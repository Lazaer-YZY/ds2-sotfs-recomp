/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/vulkan/shared_memory.h>

#include <chrono>
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(vulkan_sparse_shared_memory, true, "GPU/Vulkan",
                    "Use sparse shared memory on Vulkan")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vulkan_shared_memory_hoist_uploads, true, "GPU/Vulkan",
                    "Copy guest memory pages that nothing in the current submission has used yet at the start "
                    "of the submission, with one pair of barriers, instead of a barrier pair and a render pass "
                    "break per upload")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, vulkan_gpu_timing);

namespace rex::graphics::vulkan {

VulkanSharedMemory::VulkanSharedMemory(VulkanCommandProcessor& command_processor,
                                       memory::Memory& memory,
                                       VkPipelineStageFlags guest_shader_pipeline_stages)
    : SharedMemory(memory),
      command_processor_(command_processor),
      guest_shader_pipeline_stages_(guest_shader_pipeline_stages) {}

VulkanSharedMemory::~VulkanSharedMemory() {
  Shutdown(true);
}

bool VulkanSharedMemory::Initialize() {
  InitializeCommon();

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  const VkBufferCreateFlags sparse_flags =
      VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;

  // Try to create a sparse buffer.
  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = sparse_flags;
  buffer_create_info.size = kBufferSize;
  buffer_create_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_create_info.queueFamilyIndexCount = 0;
  buffer_create_info.pQueueFamilyIndices = nullptr;
  if (REXCVAR_GET(vulkan_sparse_shared_memory) &&
      vulkan_device->properties().sparseResidencyBuffer) {
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) == VK_SUCCESS) {
      VkMemoryRequirements buffer_memory_requirements;
      dfn.vkGetBufferMemoryRequirements(device, buffer_, &buffer_memory_requirements);
      if (rex::bit_scan_forward(buffer_memory_requirements.memoryTypeBits &
                                    vulkan_device->memory_types().device_local,
                                &buffer_memory_type_)) {
        uint32_t allocation_size_log2;
        rex::bit_scan_forward(std::max(uint64_t(buffer_memory_requirements.alignment), uint64_t(1)),
                              &allocation_size_log2);
        if (allocation_size_log2 < kBufferSizeLog2) {
          // Maximum of 1024 allocations in the worst case for all of the
          // buffer because of the overall 4096 allocation count limit on
          // Windows drivers.
          InitializeSparseHostGpuMemory(std::max(
              allocation_size_log2,
              std::max(kHostGpuMemoryOptimalSparseAllocationLog2, kBufferSizeLog2 - uint32_t(10))));
        } else {
          // Shouldn't happen on any real platform, but no point allocating the
          // buffer sparsely.
          dfn.vkDestroyBuffer(device, buffer_, nullptr);
          buffer_ = VK_NULL_HANDLE;
        }
      } else {
        REXGPU_ERROR(
            "Shared memory: Failed to get a device-local Vulkan memory type "
            "for the sparse buffer");
        dfn.vkDestroyBuffer(device, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
      }
    } else {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB Vulkan sparse buffer",
                   kBufferSize >> 20);
    }
  }

  // Create a non-sparse buffer if there were issues with the sparse buffer.
  if (buffer_ == VK_NULL_HANDLE) {
    REXGPU_INFO(
        "Vulkan sparse binding is not used for shared memory emulation - video "
        "memory usage may increase significantly because a full {} MB buffer "
        "will be created",
        kBufferSize >> 20);
    buffer_create_info.flags &= ~sparse_flags;
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) != VK_SUCCESS) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB Vulkan buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
    VkMemoryRequirements buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, buffer_, &buffer_memory_requirements);
    if (!rex::bit_scan_forward(
            buffer_memory_requirements.memoryTypeBits & vulkan_device->memory_types().device_local,
            &buffer_memory_type_)) {
      REXGPU_ERROR(
          "Shared memory: Failed to get a device-local Vulkan memory type for "
          "the buffer");
      Shutdown();
      return false;
    }
    VkMemoryAllocateInfo buffer_memory_allocate_info;
    VkMemoryAllocateInfo* buffer_memory_allocate_info_last = &buffer_memory_allocate_info;
    buffer_memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buffer_memory_allocate_info.pNext = nullptr;
    buffer_memory_allocate_info.allocationSize = buffer_memory_requirements.size;
    buffer_memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
    VkMemoryDedicatedAllocateInfo buffer_memory_dedicated_allocate_info;
    if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
      buffer_memory_allocate_info_last->pNext = &buffer_memory_dedicated_allocate_info;
      buffer_memory_allocate_info_last =
          reinterpret_cast<VkMemoryAllocateInfo*>(&buffer_memory_dedicated_allocate_info);
      buffer_memory_dedicated_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
      buffer_memory_dedicated_allocate_info.pNext = nullptr;
      buffer_memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
      buffer_memory_dedicated_allocate_info.buffer = buffer_;
    }
    VkDeviceMemory buffer_memory;
    if (dfn.vkAllocateMemory(device, &buffer_memory_allocate_info, nullptr, &buffer_memory) !=
        VK_SUCCESS) {
      REXGPU_ERROR(
          "Shared memory: Failed to allocate {} MB of memory for the Vulkan "
          "buffer",
          kBufferSize >> 20);
      Shutdown();
      return false;
    }
    buffer_memory_.push_back(buffer_memory);
    if (dfn.vkBindBufferMemory(device, buffer_, buffer_memory, 0) != VK_SUCCESS) {
      REXGPU_ERROR("Shared memory: Failed to bind memory to the Vulkan buffer");
      Shutdown();
      return false;
    }
  }

  // The first usage will likely be uploading.
  last_usage_ = Usage::kTransferDestination;
  last_written_range_ = std::make_pair<uint32_t, uint32_t>(0, 0);

  upload_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      rex::align(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize, size_t(1)
                                                                           << page_size_log2()));

  return true;
}

void VulkanSharedMemory::Shutdown(bool from_destructor) {
  upload_buffer_pool_.reset();

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, buffer_);
  for (VkDeviceMemory memory : buffer_memory_) {
    dfn.vkFreeMemory(device, memory, nullptr);
  }
  buffer_memory_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanSharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void VulkanSharedMemory::EndSubmission() {
  upload_buffer_pool_->FlushWrites();
}

void VulkanSharedMemory::Use(Usage usage, std::pair<uint32_t, uint32_t> written_range,
                             bool coalesce_writes) {
  written_range.first = std::min(written_range.first, kBufferSize);
  written_range.second = std::min(written_range.second, kBufferSize - written_range.first);
  assert_true(usage != Usage::kRead || !written_range.second);
  // Upload hoisting: GPU writes recorded from here on (resolves, lazy resolve dumps, memexport) touch
  // their pages in this submission, so later uploads of them stay in order after the writes. Uploads
  // (kTransferDestination) only cover pages RequestRanges has touched already.
  if (usage != Usage::kTransferDestination && written_range.second) {
    TouchRange(written_range.first, written_range.second);
  }
  // Consecutive memexport draws that don't read what the previous ones wrote (the caller checks
  // its vertex fetches against pending_written_range()): no barrier between them, the written
  // ranges accumulate and the next read, or a draw that reads them, gets one barrier for all.
  // Lightning Returns' battle starts run ~10,000 such draws; a barrier each ends the render pass
  // every time on a tiler.
  if (coalesce_writes && usage == Usage::kGuestDrawReadWrite && last_usage_ == usage &&
      last_written_range_.second && written_range.second) {
    const uint32_t start = std::min(last_written_range_.first, written_range.first);
    const uint32_t end = std::max(last_written_range_.first + last_written_range_.second,
                                  written_range.first + written_range.second);
    last_written_range_ = std::make_pair(start, end - start);
    return;
  }
  if (last_usage_ != usage || last_written_range_.second) {
    // DS2 diagnostic (with vulkan_gpu_timing): which shared memory usage changes cost barriers (each one ends the
    // current render pass on a tiler), counted from -> to and logged every 10 s.
    if (REXCVAR_GET(vulkan_gpu_timing)) {
      static uint32_t transitions[4][4] = {};
      static auto last_log = std::chrono::steady_clock::now();
      ++transitions[uint32_t(last_usage_)][uint32_t(usage)];
      const auto now = std::chrono::steady_clock::now();
      if (now - last_log >= std::chrono::seconds(10)) {
        static const char* const kNames[4] = {"read", "draw", "compute-write", "transfer-dst"};
        std::string line;
        for (uint32_t f = 0; f < 4; ++f) {
          for (uint32_t t = 0; t < 4; ++t) {
            if (transitions[f][t]) line += fmt::format(" {}->{}: {}", kNames[f], kNames[t], transitions[f][t]);
            transitions[f][t] = 0;
          }
        }
        REXGPU_INFO("Shared memory barriers in 10 s:{}", line);
        last_log = now;
      }
    }
    VkPipelineStageFlags src_stage_mask, dst_stage_mask;
    VkAccessFlags src_access_mask, dst_access_mask;
    GetUsageMasks(last_usage_, src_stage_mask, src_access_mask);
    GetUsageMasks(usage, dst_stage_mask, dst_access_mask);
    VkDeviceSize offset, size;
    if (last_usage_ == usage) {
      // Committing the previous write, while not changing the access mask
      // (passing false as whether to skip the barrier if no masks are changed
      // for this reason).
      offset = VkDeviceSize(last_written_range_.first);
      size = VkDeviceSize(last_written_range_.second);
    } else {
      // Changing the stage and access mask - all preceding writes must be
      // available not only to the source stage, but to the destination as well.
      offset = 0;
      size = VK_WHOLE_SIZE;
      last_usage_ = usage;
    }
    command_processor_.PushBufferMemoryBarrier(
        buffer_, offset, size, src_stage_mask, dst_stage_mask, src_access_mask, dst_access_mask,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  }
  last_written_range_ = written_range;
}

bool VulkanSharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                          uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize = length_allocations
                                        << host_gpu_memory_sparse_granularity_log2();
  memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) != VK_SUCCESS) {
    REXGPU_ERROR("Shared memory: Failed to allocate sparse buffer memory");
    return false;
  }
  buffer_memory_.push_back(memory);

  VkSparseMemoryBind bind;
  bind.resourceOffset = offset_allocations << host_gpu_memory_sparse_granularity_log2();
  bind.size = memory_allocate_info.allocationSize;
  bind.memory = memory;
  bind.memoryOffset = 0;
  bind.flags = 0;
  VkPipelineStageFlags bind_wait_stage_mask =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
      VK_PIPELINE_STAGE_TRANSFER_BIT;
  if (vulkan_device->properties().tessellationShader) {
    bind_wait_stage_mask |= VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
  }
  command_processor_.SparseBindBuffer(buffer_, 1, &bind, bind_wait_stage_mask);

  return true;
}

bool VulkanSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  ++command_processor_.gpu_timing_counters().uploads;
  command_processor_.GpuTrace("U");
  // upload_page_ranges are sorted, use them to determine the range for the
  // ordering barrier.
  Use(Usage::kTransferDestination,
      std::make_pair(upload_page_ranges.front().first << page_size_log2(),
                     (upload_page_ranges.back().first + upload_page_ranges.back().second -
                      upload_page_ranges.front().first)
                         << page_size_log2()));
  command_processor_.SubmitBarriers(true);
  command_processor_.GpuTimingMark(VulkanCommandProcessor::GpuTimingCategory::kUploads);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  uint64_t submission_current = command_processor_.GetCurrentSubmission();
  bool successful = true;
  upload_regions_.clear();
  VkBuffer upload_buffer_previous = VK_NULL_HANDLE;
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    while (upload_range_length) {
      VkBuffer upload_buffer;
      VkDeviceSize upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          submission_current, upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), upload_buffer, upload_buffer_offset, upload_buffer_size);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get a Vulkan upload buffer");
        successful = false;
        break;
      }
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      std::memcpy(upload_buffer_mapping,
                  memory().TranslatePhysical(upload_range_start << page_size_log2()),
                  upload_buffer_size);
      if (upload_buffer_previous != upload_buffer && !upload_regions_.empty()) {
        assert_true(upload_buffer_previous != VK_NULL_HANDLE);
        command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                       uint32_t(upload_regions_.size()), upload_regions_.data());
        upload_regions_.clear();
      }
      upload_buffer_previous = upload_buffer;
      VkBufferCopy& upload_region = upload_regions_.emplace_back();
      upload_region.srcOffset = upload_buffer_offset;
      upload_region.dstOffset = VkDeviceSize(upload_range_start << page_size_log2());
      upload_region.size = upload_buffer_size;
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
    if (!successful) {
      break;
    }
  }
  if (!upload_regions_.empty()) {
    assert_true(upload_buffer_previous != VK_NULL_HANDLE);
    command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                   uint32_t(upload_regions_.size()), upload_regions_.data());
    upload_regions_.clear();
  }
  return successful;
}

uint64_t VulkanSharedMemory::UploadHoistEpoch() const {
  const uint64_t submission = command_processor_.GetCurrentSubmission();
  if (!REXCVAR_GET(vulkan_shared_memory_hoist_uploads)) {
    hoist_enabled_since_submission_ = UINT64_MAX;
    return 0;
  }
  // Switched on (or first seen) while a submission may already have untracked requests and writes:
  // start with the next submission, so every page requested or written in a hoisting submission
  // has been tracked from its beginning.
  if (hoist_enabled_since_submission_ == UINT64_MAX) {
    hoist_enabled_since_submission_ = submission + 1;
  }
  return submission >= hoist_enabled_since_submission_ ? submission : 0;
}

bool VulkanSharedMemory::UploadRangesHoisted(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  command_processor_.GpuTrace("Uh");
  // No barrier and no deferred command: the copies are recorded by RecordHoistedUploads at the
  // beginning of this submission's command buffer.
  const uint64_t submission_current = command_processor_.GetCurrentSubmission();
  if (hoisted_copies_submission_ != submission_current) {
    hoisted_copies_.clear();
    hoisted_copies_submission_ = submission_current;
  }
  bool successful = true;
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    while (upload_range_length) {
      VkBuffer upload_buffer;
      VkDeviceSize upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          submission_current, upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), upload_buffer, upload_buffer_offset, upload_buffer_size);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get a Vulkan upload buffer");
        successful = false;
        break;
      }
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      std::memcpy(upload_buffer_mapping,
                  memory().TranslatePhysical(upload_range_start << page_size_log2()),
                  upload_buffer_size);
      VkBufferCopy upload_region;
      upload_region.srcOffset = upload_buffer_offset;
      upload_region.dstOffset = VkDeviceSize(upload_range_start << page_size_log2());
      upload_region.size = upload_buffer_size;
      hoisted_copies_.emplace_back(upload_buffer, upload_region);
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
    if (!successful) {
      break;
    }
  }
  return successful;
}

uint64_t VulkanSharedMemory::RecordHoistedUploads(VkCommandBuffer command_buffer) {
  if (hoisted_copies_.empty()) {
    return 0;
  }
  if (hoisted_copies_submission_ != command_processor_.GetCurrentSubmission()) {
    // Left over from a submission that has been submitted already (they are kept until then, so a
    // retried EndSubmission records them again): never record them into a later one.
    hoisted_copies_.clear();
    return 0;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn =
      command_processor_.GetVulkanDevice()->functions();
  // Earlier submissions may still read or write these pages (write-after-read needs the execution
  // dependency only, write-after-write the availability of the earlier writes).
  VkMemoryBarrier barrier;
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.pNext = nullptr;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  uint64_t bytes = 0;
  size_t i = 0;
  while (i < hoisted_copies_.size()) {
    const VkBuffer source = hoisted_copies_[i].first;
    upload_regions_.clear();
    for (; i < hoisted_copies_.size() && hoisted_copies_[i].first == source; ++i) {
      upload_regions_.push_back(hoisted_copies_[i].second);
      bytes += hoisted_copies_[i].second.size;
    }
    dfn.vkCmdCopyBuffer(command_buffer, source, buffer_, uint32_t(upload_regions_.size()),
                        upload_regions_.data());
  }
  upload_regions_.clear();
  // Everything in the submission after the copies, whatever the usage tracked by Use is: the union
  // of the stages and accesses of all usages (index, vertex fetch / texel buffer and memexport in
  // guest shaders, compute reads and writes - texture loads, resolves -, transfer reads and writes).
  VkPipelineStageFlags dst_stage_mask = 0;
  VkAccessFlags dst_access_mask = 0;
  for (Usage usage : {Usage::kRead, Usage::kGuestDrawReadWrite, Usage::kComputeWrite,
                      Usage::kTransferDestination}) {
    VkPipelineStageFlags usage_stage_mask;
    VkAccessFlags usage_access_mask;
    GetUsageMasks(usage, usage_stage_mask, usage_access_mask);
    dst_stage_mask |= usage_stage_mask;
    dst_access_mask |= usage_access_mask;
  }
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = dst_access_mask;
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage_mask, 0, 1,
                           &barrier, 0, nullptr, 0, nullptr);

  // Diagnostic (with vulkan_gpu_timing), logged every 10 s.
  if (REXCVAR_GET(vulkan_gpu_timing)) {
    static uint64_t log_submissions = 0, log_copies = 0, log_bytes = 0;
    static auto last_log = std::chrono::steady_clock::now();
    ++log_submissions;
    log_copies += hoisted_copies_.size();
    log_bytes += bytes;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log >= std::chrono::seconds(10)) {
      REXGPU_INFO("Shared memory hoisted uploads in 10 s: {} copies, {} KB, in {} submissions",
                  log_copies, log_bytes >> 10, log_submissions);
      log_submissions = 0;
      log_copies = 0;
      log_bytes = 0;
      last_log = now;
    }
  }
  return bytes;
}

void VulkanSharedMemory::GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                                       VkAccessFlags& access_mask) const {
  switch (usage) {
    case Usage::kComputeWrite:
      stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      // Was SHADER_READ only (as in Xenia), so barriers after resolves never
      // made the resolve shaders' writes available; the Adreno driver then let
      // texture loads read stale resolved memory.
      access_mask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      return;
    case Usage::kTransferDestination:
      stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
      return;
    default:
      break;
  }
  stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_;
  access_mask = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  switch (usage) {
    case Usage::kRead:
      stage_mask |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask |= VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case Usage::kGuestDrawReadWrite:
      access_mask |= VK_ACCESS_SHADER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

}  // namespace rex::graphics::vulkan
