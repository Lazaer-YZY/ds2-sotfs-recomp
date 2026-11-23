#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <unordered_set>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/graphics/vulkan/pipeline_cache.h>
#include <rex/graphics/vulkan/primitive_processor.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/graphics/vulkan/texture_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/vulkan/linked_type_descriptor_set_allocator.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor : public CommandProcessor {
 public:
  // Single-descriptor layouts for use within a single frame.
  enum class SingleTransientDescriptorLayout {
    kStorageBufferCompute,
    kStorageBufferPairCompute,
    kCount,
  };

  class ScratchBufferAcquisition {
   public:
    explicit ScratchBufferAcquisition() = default;
    explicit ScratchBufferAcquisition(VulkanCommandProcessor& command_processor, VkBuffer buffer,
                                      VkPipelineStageFlags stage_mask, VkAccessFlags access_mask)
        : command_processor_(&command_processor),
          buffer_(buffer),
          stage_mask_(stage_mask),
          access_mask_(access_mask) {}

    ScratchBufferAcquisition(const ScratchBufferAcquisition& acquisition) = delete;
    ScratchBufferAcquisition& operator=(const ScratchBufferAcquisition& acquisition) = delete;

    ScratchBufferAcquisition(ScratchBufferAcquisition&& acquisition) {
      command_processor_ = acquisition.command_processor_;
      buffer_ = acquisition.buffer_;
      stage_mask_ = acquisition.stage_mask_;
      access_mask_ = acquisition.access_mask_;
      acquisition.command_processor_ = nullptr;
      acquisition.buffer_ = VK_NULL_HANDLE;
      acquisition.stage_mask_ = 0;
      acquisition.access_mask_ = 0;
    }
    ScratchBufferAcquisition& operator=(ScratchBufferAcquisition&& acquisition) {
      if (this == &acquisition) {
        return *this;
      }
      command_processor_ = acquisition.command_processor_;
      buffer_ = acquisition.buffer_;
      stage_mask_ = acquisition.stage_mask_;
      access_mask_ = acquisition.access_mask_;
      acquisition.command_processor_ = nullptr;
      acquisition.buffer_ = VK_NULL_HANDLE;
      acquisition.stage_mask_ = 0;
      acquisition.access_mask_ = 0;
      return *this;
    }

    ~ScratchBufferAcquisition() {
      if (buffer_ != VK_NULL_HANDLE) {
        assert_true(command_processor_->scratch_buffer_used_);
        assert_true(command_processor_->scratch_buffer_ == buffer_);
        command_processor_->scratch_buffer_last_stage_mask_ = stage_mask_;
        command_processor_->scratch_buffer_last_access_mask_ = access_mask_;
        command_processor_->scratch_buffer_last_usage_submission_ =
            command_processor_->GetCurrentSubmission();
        command_processor_->scratch_buffer_used_ = false;
      }
    }

    // VK_NULL_HANDLE if failed to acquire or if moved.
    VkBuffer buffer() const { return buffer_; }

    VkPipelineStageFlags GetStageMask() const { return stage_mask_; }
    VkPipelineStageFlags SetStageMask(VkPipelineStageFlags new_stage_mask) {
      VkPipelineStageFlags old_stage_mask = stage_mask_;
      stage_mask_ = new_stage_mask;
      return old_stage_mask;
    }
    VkAccessFlags GetAccessMask() const { return access_mask_; }
    VkAccessFlags SetAccessMask(VkAccessFlags new_access_mask) {
      VkAccessFlags old_access_mask = access_mask_;
      access_mask_ = new_access_mask;
      return old_access_mask;
    }

   private:
    VulkanCommandProcessor* command_processor_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkPipelineStageFlags stage_mask_ = 0;
    VkAccessFlags access_mask_ = 0;
  };

  VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~VulkanCommandProcessor();

  void ClearCaches() override;
  void InvalidateGpuMemory() override;
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

  // The driver pipeline cache file merged at setup, before the built-in pipelines were created
  // (empty if none), and its merged size (0 if the file was missing or discarded).
  const std::filesystem::path& setup_driver_pipeline_cache_path() const {
    return setup_driver_pipeline_cache_path_;
  }
  size_t setup_driver_pipeline_cache_size() const { return setup_driver_pipeline_cache_size_; }

  ui::vulkan::VulkanDevice* GetVulkanDevice() const {
    return static_cast<const ui::vulkan::VulkanProvider*>(graphics_system_->provider())
        ->vulkan_device();
  }

  bool CompileGlslToSpirv(VkShaderStageFlagBits stage, std::string_view source,
                          std::vector<uint32_t>& spirv_out, std::string& error_out) const;

  // Returns the deferred drawing command list for the currently open
  // submission.
  DeferredCommandBuffer& deferred_command_buffer() {
    assert_true(submission_open_);
    return deferred_command_buffer_;
  }

  bool submission_open() const { return submission_open_; }
  // Null before the context is set up and after it's shut down.
  VulkanRenderTargetCache* render_target_cache() const { return render_target_cache_.get(); }
  uint64_t GetCurrentSubmission() const {
    return submission_completed_ + uint64_t(submissions_in_flight_fences_.size()) + 1;
  }
  uint64_t GetCompletedSubmission() const { return submission_completed_; }

  // Sparse binds are:
  // - In a single submission, all submitted in one vkQueueBindSparse.
  // - Sent to the queue without waiting for a semaphore.
  // Thus, multiple sparse binds between the completed and the current
  // submission, and within one submission, must not touch any overlapping
  // memory regions.
  void SparseBindBuffer(VkBuffer buffer, uint32_t bind_count, const VkSparseMemoryBind* binds,
                        VkPipelineStageFlags wait_stage_mask);

  uint64_t GetCurrentFrame() const { return frame_current_; }
  uint64_t GetCompletedFrame() const { return frame_completed_; }

  // Submission must be open to insert barriers. If no pipeline stages access
  // the resource in a synchronization scope, the stage masks should be 0 (top /
  // bottom of pipe should be specified only if explicitly needed). Returning
  // true if the barrier has actually been inserted and not dropped.
  bool PushBufferMemoryBarrier(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                               VkPipelineStageFlags src_stage_mask,
                               VkPipelineStageFlags dst_stage_mask, VkAccessFlags src_access_mask,
                               VkAccessFlags dst_access_mask,
                               uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                               uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                               bool skip_if_equal = true);
  bool PushImageMemoryBarrier(VkImage image, const VkImageSubresourceRange& subresource_range,
                              VkPipelineStageFlags src_stage_mask,
                              VkPipelineStageFlags dst_stage_mask, VkAccessFlags src_access_mask,
                              VkAccessFlags dst_access_mask, VkImageLayout old_layout,
                              VkImageLayout new_layout,
                              uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                              uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                              bool skip_if_equal = true);
  // Returns whether any barriers have been submitted - if true is returned, the
  // render pass will also be closed.
  bool SubmitBarriers(bool force_end_render_pass);

  // If not started yet, begins a render pass from the render target cache.
  // Submission must be open.
  void AttachmentBarrierBetweenPasses();
  bool IsInRenderTargetCacheRenderPass(
      const VulkanRenderTargetCache::Framebuffer* framebuffer) const {
    return in_render_pass_ && current_framebuffer_ == framebuffer;
  }
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer);
  // Overload for transfer operations with dynamic rendering.
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer,
      VkImageView transfer_dest_view, bool transfer_dest_is_depth);
  // Must be called before doing anything outside the render pass scope,
  // including adding pipeline barriers that are not a part of the render pass
  // scope. Submission must be open.
  void EndRenderPass();
  // Must be called (after entering the render target cache render pass) for
  // every rectangle (in framebuffer pixels, including the resolution scale)
  // that a command recorded in the current render target cache render pass
  // instance may write: the scissor of a draw, the geometry of a transfer draw
  // whose scissor is the whole framebuffer, the rectangles of
  // vkCmdClearAttachments. With vulkan_render_area_restrict, the render area of
  // the instance is shrunk to the tile-aligned union of these rectangles when
  // the instance ends (the begin command is patched in the deferred command
  // buffer, so the render pass instance is never split). No-op outside a
  // render target cache render pass.
  void ExtendRenderPassArea(const VkRect2D& rect);

  // Optional GPU timing breakdown (vulkan_gpu_timing). GPU work is split into
  // segments by timestamps written only outside render passes (a timestamp
  // inside a pass could perturb tiling on mobile GPUs), so a guest render pass
  // is timed as a whole, including its attachment loads and stores.
  enum class GpuTimingCategory : uint8_t {
    kDraws,
    kTransfers,
    kResolves,
    kTextureLoads,
    kUploads,
    kSwap,
    kOther,
    kCount,
  };
  // Starts a segment of the category before GPU work recorded outside a render
  // pass (call after SubmitBarriers). No-op unless timing this submission.
  void GpuTimingMark(GpuTimingCategory category) {
    if (gpu_timing_current_.pool != VK_NULL_HANDLE) {
      GpuTimingWriteTimestamp(category);
    }
  }
  // Render target ownership transfers (and host depth stores) done as a part
  // of a resolve are counted as resolves.
  GpuTimingCategory GpuTimingTransferCategory() const {
    return gpu_timing_in_resolve_ ? GpuTimingCategory::kResolves : GpuTimingCategory::kTransfers;
  }
  // Signature of the next render pass instance for the per-signature GPU time
  // breakdown: RenderPassKey | pitch tiles at 32bpp << 32; bit 63 marks an
  // ownership transfer pass, bit 62 a resolve clear pass.
  void GpuTimingSetPassTag(uint64_t tag) { gpu_timing_pass_tag_ = tag; }
  // Resolve / texture load being recorded, for GPU time by operation and size:
  // kind ('R' or 'T') << 56 | is depth << 55 | format << 48 | width << 24 | height.
  uint64_t gpu_timing_op_tag() const { return gpu_timing_op_tag_; }
  void GpuTimingRestoreOpTag(uint64_t tag) { gpu_timing_op_tag_ = tag; }
  void GpuTimingSetOpTag(uint64_t tag) {
    gpu_timing_op_tag_ = tag;
    if (gpu_timing_enabled_) {
      ++gpu_timing_op_stats_[tag].passes;
    }
  }
  // vulkan_gpu_trace_frame: the command sequence of one frame per timing window.
  bool gpu_trace_active() const { return gpu_trace_active_; }
  void GpuTrace(std::string_view event) {
    if (gpu_trace_active_) {
      gpu_trace_ += event;
      gpu_trace_ += ' ';
    }
  }
  // pair: host depth source << 32 | source descriptor << 16 | destination
  // descriptor (is_depth << 15 | msaa << 13 | resource format << 8 | pitch).
  void GpuTimingNoteTransfer(uint64_t pair, uint32_t tiles) {
    if (gpu_timing_enabled_) {
      auto& t = gpu_timing_transfer_pairs_[pair];
      ++t.first;
      t.second += tiles;
    }
  }
  // Cheap CPU counters, always maintained, logged with vulkan_gpu_timing.
  struct GpuTimingCounters {
    uint64_t render_passes = 0;
    uint64_t render_passes_with_load = 0;
    uint64_t transfers = 0;
    uint64_t resolves = 0;
    uint64_t texture_loads = 0;
    uint64_t uploads = 0;
    uint64_t draws = 0;
    uint64_t submissions = 0;
    // Render target cache render pass instances (guest and transfer) and their
    // render areas (vulkan_render_area_restrict), summed over the instances.
    uint64_t render_area_passes = 0;
    uint64_t render_area_restricted_passes = 0;
    uint64_t render_area_width = 0;
    uint64_t render_area_height = 0;
    uint64_t render_area_pixels = 0;
    // Tile-aligned union of what the commands in the instances may write,
    // regardless of whether the render area has actually been restricted.
    uint64_t render_area_touched_pixels = 0;
    uint64_t render_area_framebuffer_width = 0;
    uint64_t render_area_framebuffer_height = 0;
    uint64_t render_area_framebuffer_pixels = 0;
    // Render pass instance ends by PassEndReason.
    uint64_t pass_end_reasons[8] = {};
  };
  GpuTimingCounters& gpu_timing_counters() { return gpu_timing_counters_; }

  VkDescriptorSetLayout GetSingleTransientDescriptorLayout(
      SingleTransientDescriptorLayout transient_descriptor_layout) const {
    return descriptor_set_layouts_single_transient_[size_t(transient_descriptor_layout)];
  }
  // A frame must be open.
  VkDescriptorSet AllocateSingleTransientDescriptor(
      SingleTransientDescriptorLayout transient_descriptor_layout);

  // The returned reference is valid until a cache clear.
  VkDescriptorSetLayout GetTextureDescriptorSetLayout(bool is_vertex, size_t texture_count,
                                                      size_t sampler_count);
  // The returned reference is valid until a cache clear.
  const VulkanPipelineCache::PipelineLayoutProvider* GetPipelineLayout(size_t texture_count_pixel,
                                                                       size_t sampler_count_pixel,
                                                                       size_t texture_count_vertex,
                                                                       size_t sampler_count_vertex);

  // Returns a single temporary GPU-side buffer within a submission for tasks
  // like texture untiling and resolving. May push a buffer memory barrier into
  // the initial usage. Submission must be open.
  ScratchBufferAcquisition AcquireScratchGpuBuffer(VkDeviceSize size,
                                                   VkPipelineStageFlags initial_stage_mask,
                                                   VkAccessFlags initial_access_mask);

  // Binds a graphics pipeline for host-specific purposes, invalidating the
  // affected state. keep_dynamic_* must be false (to invalidate the dynamic
  // state after binding the pipeline with the same state being static, or if
  // the caller changes the dynamic state bypassing the VulkanCommandProcessor)
  // unless the caller has these state variables as dynamic and uses the
  // tracking in VulkanCommandProcessor to modify them.
  void BindExternalGraphicsPipeline(VkPipeline pipeline, bool keep_dynamic_depth_bias = false,
                                    bool keep_dynamic_blend_constants = false,
                                    bool keep_dynamic_stencil_mask_ref = false);
  void BindExternalComputePipeline(VkPipeline pipeline);
  void SetViewport(const VkViewport& viewport);
  void SetScissor(const VkRect2D& scissor);

  // Returns the text to display in the GPU backend name in the window title.
  std::string GetWindowTitleText() const;

  // The last swaps (vulkan_scene_fxaa): the presented frame's size and the recent front buffer addresses, so that a
  // full-screen resolve can be told apart from the one into the front buffer.
  uint32_t last_swap_width() const { return last_swap_width_; }
  uint32_t last_swap_height() const { return last_swap_height_; }
  // The scene for vulkan_scene_fxaa: the last full-screen k_8_8_8_8 resolve (not into a front buffer) before the
  // swap, remembered at the swap; earlier ones in the frame are the G-buffer and other passes' data.
  void NoteSceneFxaaCandidate(uint32_t address) { scene_fxaa_candidate_ = address; }
  uint32_t scene_fxaa_address() const { return scene_fxaa_address_; }
  bool IsRecentFrontBuffer(uint32_t address) const {
    for (uint32_t frontbuffer : recent_frontbuffers_) {
      if (frontbuffer && ((frontbuffer ^ address) & 0x1FFFFFFFu) == 0) return true;
    }
    return false;
  }

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;
  bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count) override;

  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;
  void OnPrimaryBufferEnd() override;
  void PrepareForWait() override;
  // vulkan_depth_4x_as_1x: rewrites RB_SURFACE_INFO for the current draw (IssueDraw restores it).
  void RewriteDepth4xAs1x(const VulkanShader* pixel_shader);
  uint32_t draw_samples_as_pixels_shift_ = 0;
  std::unordered_set<uint32_t> depth_4x_scene_surfaces_;
  uint64_t depth_4x_as_1x_draws_ = 0;

 public:
  void NoteFenceWait(void* caller, uint64_t us);

 protected:
  struct FenceWaitStats {
    std::chrono::steady_clock::time_point start{};
    uint64_t waits = 0, us = 0, max_us = 0;
    std::array<std::pair<void*, uint64_t>, 6> callers{};
  };
  FenceWaitStats fence_wait_stats_;
  // vulkan_slow_frame_ms: the command thread's time between two swaps, split by activity.
  struct CpFrameStats {
    uint64_t start_ns = 0, translate_ns = 0, textures_ns = 0, pipeline_ns = 0, rt_update_ns = 0,
             copy_ns = 0, submit_ns = 0, fence_us = 0, regmem_us_start = 0, primitives_ns = 0,
             vertex_buffers_ns = 0, vertex_buffer_bytes = 0, memexport_ns = 0, memexport_bytes = 0,
             present_ns = 0;
    uint32_t draws = 0, copies = 0, memexport_draws = 0, memexport_unknown_draws = 0;
    // The thread's own clocks at the frame start: stall (waiting for the game's commands), CPU time, run queue.
    uint64_t stall_ns_start = 0, cpu_ns_start = 0, run_ns_start = 0, queue_ns_start = 0;
  };
  CpFrameStats cp_frame_;
  void CpFrameOnSwap();

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  uint32_t last_swap_width_ = 0, last_swap_height_ = 0;
  uint32_t recent_frontbuffers_[4] = {};
  uint32_t recent_frontbuffer_next_ = 0;
  uint32_t scene_fxaa_candidate_ = 0, scene_fxaa_address_ = 0;
  std::filesystem::path setup_driver_pipeline_cache_path_;
  size_t setup_driver_pipeline_cache_size_ = 0;

  struct CommandBuffer {
    VkCommandPool pool;
    VkCommandBuffer buffer;
  };

  struct SparseBufferBind {
    VkBuffer buffer;
    size_t bind_offset;
    uint32_t bind_count;
  };

  union TextureDescriptorSetLayoutKey {
    uint32_t key;
    struct {
      // If texture and sampler counts are both 0, use
      // descriptor_set_layout_empty_ instead as these are owning references.
      uint32_t texture_count : 16;
      uint32_t sampler_count : 15;
      uint32_t is_vertex : 1;
    };

    TextureDescriptorSetLayoutKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TextureDescriptorSetLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const TextureDescriptorSetLayoutKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const TextureDescriptorSetLayoutKey& other_key) const {
      return !(*this == other_key);
    }
  };

  union PipelineLayoutKey {
    uint64_t key;
    struct {
      // Pixel textures in the low bits since those are varied much more
      // commonly.
      uint16_t texture_count_pixel;
      uint16_t sampler_count_pixel;
      uint16_t texture_count_vertex;
      uint16_t sampler_count_vertex;
    };

    PipelineLayoutKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const PipelineLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const PipelineLayoutKey& other_key) const { return key == other_key.key; }
    bool operator!=(const PipelineLayoutKey& other_key) const { return !(*this == other_key); }
  };

  class PipelineLayout : public VulkanPipelineCache::PipelineLayoutProvider {
   public:
    explicit PipelineLayout(VkPipelineLayout pipeline_layout,
                            VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref,
                            VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref,
                            VkPipelineLayout pre_rasterization_library_layout = VK_NULL_HANDLE,
                            VkPipelineLayout fragment_library_layout = VK_NULL_HANDLE)
        : pipeline_layout_(pipeline_layout),
          descriptor_set_layout_textures_vertex_ref_(descriptor_set_layout_textures_vertex_ref),
          descriptor_set_layout_textures_pixel_ref_(descriptor_set_layout_textures_pixel_ref),
          pre_rasterization_library_layout_(pre_rasterization_library_layout),
          fragment_library_layout_(fragment_library_layout) {}
    VkPipelineLayout GetPipelineLayout() const override { return pipeline_layout_; }
    VkPipelineLayout GetPreRasterizationLibraryLayout() const override {
      return pre_rasterization_library_layout_;
    }
    VkPipelineLayout GetFragmentLibraryLayout() const override { return fragment_library_layout_; }
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref() const {
      return descriptor_set_layout_textures_vertex_ref_;
    }
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref() const {
      return descriptor_set_layout_textures_pixel_ref_;
    }

   private:
    VkPipelineLayout pipeline_layout_;
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref_;
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref_;
    VkPipelineLayout pre_rasterization_library_layout_;
    VkPipelineLayout fragment_library_layout_;
  };

  struct UsedSingleTransientDescriptor {
    uint64_t frame;
    SingleTransientDescriptorLayout layout;
    VkDescriptorSet set;
  };

  struct UsedTextureTransientDescriptorSet {
    uint64_t frame;
    TextureDescriptorSetLayoutKey layout;
    VkDescriptorSet set;
  };

  enum SwapApplyGammaDescriptorSet : uint32_t {
    kSwapApplyGammaDescriptorSetRamp,
    kSwapApplyGammaDescriptorSetSource,

    kSwapApplyGammaDescriptorSetCount,
  };
  enum SwapApplyGammaComputeDescriptorSet : uint32_t {
    kSwapApplyGammaComputeDescriptorSetRamp,
    kSwapApplyGammaComputeDescriptorSetSource,
    kSwapApplyGammaComputeDescriptorSetDestination,

    kSwapApplyGammaComputeDescriptorSetCount,
  };
  struct SwapApplyGammaConstants {
    uint32_t size[2];
  };
  enum SwapFxaaDescriptorSet : uint32_t {
    kSwapFxaaDescriptorSetSource,
    kSwapFxaaDescriptorSetDestination,

    kSwapFxaaDescriptorSetCount,
  };
  struct SwapFxaaConstants {
    uint32_t size[2];
    float size_inv[2];
  };

  // Framebuffer for the current presenter's guest output image revision, and
  // its usage tracking.
  struct SwapFramebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint64_t version = UINT64_MAX;
    uint64_t last_submission = 0;
  };

  // BeginSubmission and EndSubmission may be called at any time. If there's an
  // open non-frame submission, BeginSubmission(true) will promote it to a
  // frame. EndSubmission(true) will close the frame no matter whether the
  // submission has already been closed.
  // Unlike on Direct3D 12, submission boundaries do not imply any memory
  // barriers aside from an incoming host write (but not outgoing host read)
  // dependency.

  // Rechecks submission number and reclaims per-submission resources. Pass 0 as
  // the submission to await to simply check status, or pass
  // GetCurrentSubmission() to wait for all queue operations to be completed.
  void CheckSubmissionFenceAndDeviceLoss(uint64_t await_submission);
  // If is_guest_command is true, a new full frame - with full cleanup of
  // resources and, if needed, starting capturing - is opened if pending (as
  // opposed to simply resuming after mid-frame synchronization). Returns
  // whether a submission is open currently and the device is not lost.
  bool BeginSubmission(bool is_guest_command);
  // If is_swap is true, a full frame is closed - with, if needed, cache
  // clearing and stopping capturing. Returns whether the submission was done
  // successfully, if it has failed, leaves it open.
  bool EndSubmission(bool is_swap);
  bool AwaitAllQueueOperationsCompletion() {
    CheckSubmissionFenceAndDeviceLoss(GetCurrentSubmission());
    return !submission_open_ && submissions_in_flight_fences_.empty();
  }
  // Keep primary-buffer-end submit behavior aligned with D3D12: only submit
  // when immediate submission is safe.
  bool CanEndSubmissionImmediately() const;

  void ClearTransientDescriptorPools();
  // readback_mode: how the resolve result reaches guest memory (never kDisabled here).
  bool IssueCopy_ReadbackResolvePath(ReadbackResolveMode readback_mode);
  // readback_resolve_sizes: whether the resolve about to run (RB_COPY_DEST_PITCH) has one of the listed sizes.
  bool IsForcedReadbackResolveSize();
  bool IssueDraw_MemexportReadbackFullPath(uint32_t total_size);
  bool IssueDraw_MemexportReadbackFastPath(uint32_t total_size);

  void SplitPendingBarrier();

  void DestroyScratchBuffer();
  bool InitializeOcclusionQueryResources();
  void ShutdownOcclusionQueryResources();
  bool BeginGuestOcclusionQuery(uint32_t sample_count_address);
  bool EndGuestOcclusionQuery(uint32_t sample_count_address);
  bool AcquireOcclusionQueryIndex(uint32_t& host_index_out);
  void DisableHostOcclusionQueries();
  uint64_t NormalizeOcclusionSamples(uint64_t samples) const;
  void WriteGuestOcclusionResult(uint32_t sample_count_address, uint64_t samples);
  void InvalidateAllVertexBufferResidency();
  void InvalidateVertexBufferResidency(uint32_t vfetch_index);
  void InvalidateVertexBufferResidencyRange(uint32_t first_vfetch, uint32_t last_vfetch);
  struct ReadbackBuffer {
    VkBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory memories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    void* mapped_data[2] = {nullptr, nullptr};
    uint32_t sizes[2] = {0, 0};
    uint64_t submission_written[2] = {0, 0};
    uint32_t written_size[2] = {0, 0};
    uint32_t current_index = 0;
    uint64_t last_used_frame = 0;
  };
  void EvictOldReadbackBuffers(std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map);
  static constexpr uint32_t kReadbackBufferSizeIncrement = 16 * 1024 * 1024;
  static constexpr size_t kMaxReadbackBuffers = 256;
  static constexpr uint64_t kReadbackBufferEvictionAgeFrames = 60;
  static inline uint32_t AlignReadbackBufferSize(uint32_t size) {
    if (size < 1 * 1024 * 1024) {
      return rex::align(size, 256u * 1024u);
    }
    if (size < 4 * 1024 * 1024) {
      return rex::align(size, 1u * 1024u * 1024u);
    }
    return rex::align(size, kReadbackBufferSizeIncrement);
  }
  static inline uint64_t MakeReadbackResolveKey(uint32_t address, uint32_t length) {
    return (uint64_t(address) << 32) | uint64_t(length);
  }
  static inline uint64_t MakeMemexportReadbackKey(uint32_t first_base_address_dwords,
                                                  uint32_t total_size) {
    return (uint64_t(first_base_address_dwords) << 32) | uint64_t(total_size);
  }
  struct ResolveDownscaleConstants {
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t pixel_size_log2;
    uint32_t tile_count;
    uint32_t source_offset_bytes;
    uint32_t half_pixel_offset;
  };
  bool EnsureSwapFxaaSourceImage(uint32_t width, uint32_t height);
  void DestroySwapFxaaSourceImage();

  void UpdateDynamicState(const draw_util::ViewportInfo& viewport_info, bool primitive_polygonal,
                          reg::RB_DEPTHCONTROL normalized_depth_control);
  void UpdateSystemConstantValues(
      bool primitive_polygonal,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool shader_32bit_index_dma, uint32_t compute_memexport_vertex_count,
      const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask);
  bool UpdateBindings(const VulkanShader* vertex_shader, const VulkanShader* pixel_shader);
  // Allocates a descriptor set and fills one or two VkWriteDescriptorSet
  // structure instances (for images and samplers).
  // The descriptor set layout must be the one for the given is_vertex,
  // texture_count, sampler_count (from GetTextureDescriptorSetLayout - may be
  // already available at the moment of the call, no need to locate it again).
  // Returns how many VkWriteDescriptorSet structure instances have been
  // written, or 0 if there was a failure to allocate the descriptor set or no
  // bindings were requested.
  uint32_t WriteTransientTextureBindings(bool is_vertex, uint32_t texture_count,
                                         uint32_t sampler_count,
                                         VkDescriptorSetLayout descriptor_set_layout,
                                         const VkDescriptorImageInfo* texture_image_info,
                                         const VkDescriptorImageInfo* sampler_image_info,
                                         VkWriteDescriptorSet* descriptor_set_writes_out);

  bool device_lost_ = false;

  bool cache_clear_requested_ = false;

  // Host shader types that guest shaders can be translated into - they can
  // access the shared memory (via vertex fetch, memory export, or manual index
  // buffer reading) and textures.
  VkPipelineStageFlags guest_shader_pipeline_stages_ = 0;
  VkShaderStageFlags guest_shader_vertex_stages_ = 0;

  std::vector<VkFence> fences_free_;
  std::vector<VkSemaphore> semaphores_free_;

  // DS2 port diagnostic (resolve_dump_dir): every resolve of one frame copied to a host buffer and written to
  // files at the Swap, to see which pass of a frame turns black.
  // The frame capture (RequestFrameCapture from CTRL, frame_capture_after_s) is the same dump with more: each
  // resolve's EDRAM source span ('e' entries, the render target as the resolve reads it) and the render-to-texture
  // textures the frame samples ('t' entries, the host image), written to <executable folder>/logs/frame_capture.
  struct ResolveDumpEntry {
    // 'r': resolve destination (guest memory), 'e': EDRAM span, 't': host texture image.
    char kind = 'r';
    uint32_t address = 0, length = 0, offset = 0, chunk = 0;
    uint32_t copy_dest_base = 0, copy_dest_info = 0, copy_dest_pitch = 0, copy_control = 0, surface_info = 0;
    uint32_t color_info0 = 0, depth_info = 0, draws_before = 0;
    // 'e' / 't': file name and the index line's fields after it.
    std::string name, info;
  };
  void DumpResolve(uint32_t written_address, uint32_t written_length);
  void FinishResolveDump();
  // Space for one copy in the host readback chunks (allocated on demand, freed after the dump is written).
  bool ReserveDumpSpace(uint32_t length, uint32_t& chunk_out, uint32_t& offset_out);
  void FreeDumpChunks();
  // Records a copy of a buffer range (EDRAM spans) into the dump; false if out of space.
  bool DumpBufferRange(VkBuffer buffer, VkDeviceSize offset, uint32_t length, ResolveDumpEntry entry);
  // Frame capture of the textures a draw samples that resolves wrote (after RequestTextures).
  void FrameCaptureDrawTextures(uint32_t used_texture_mask);
  bool resolve_dump_active_ = false;
  uint32_t resolve_dump_count_ = 0;
  uint64_t resolve_dump_last_ms_ = 0;
  uint64_t resolve_dump_first_ms_ = 0;
  uint32_t resolve_dump_draws_ = 0;
  std::vector<ResolveDumpEntry> resolve_dump_entries_;
  std::vector<std::string> resolve_dump_draw_log_;
  struct DumpChunk {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapping = nullptr;
    uint32_t size = 0, used = 0;
  };
  std::vector<DumpChunk> resolve_dump_chunks_;
  uint64_t resolve_dump_reserved_ = 0;
  uint32_t resolve_dump_skipped_ = 0;
  // Resolves issued since the frame began (the 'r' / 'e' entries of one resolve share it).
  uint32_t resolve_dump_resolve_seq_ = 0;

 public:
  // Frame capture: the dump frame in progress is one (Resolve adds its EDRAM spans).
  bool frame_capture_active() const { return resolve_dump_active_ && frame_capture_; }
  uint32_t frame_capture_resolve_seq() const { return resolve_dump_resolve_seq_; }
  // A line in the captured frame's draws.txt (render target dumps, transfers: who owned which EDRAM tiles).
  void FrameCaptureNote(std::string line) {
    if (frame_capture_active() && resolve_dump_draw_log_.size() < 8192) {
      resolve_dump_draw_log_.push_back(std::move(line));
    }
  }
  bool FrameCaptureBufferRange(VkBuffer buffer, VkDeviceSize offset, uint32_t length, std::string name,
                               std::string info) {
    ResolveDumpEntry entry;
    entry.kind = 'e';
    entry.name = std::move(name);
    entry.info = std::move(info);
    return DumpBufferRange(buffer, offset, length, std::move(entry));
  }

 private:
  // The frame before a capture records what its resolves write (frame_capture_armed_), so textures sampled from
  // them in the captured frame are captured too; then the captured frame (resolve_dump_active_ + frame_capture_).
  bool frame_capture_ = false;
  bool frame_capture_armed_ = false;
  bool frame_capture_after_done_ = false;
  uint64_t frame_capture_first_ms_ = 0;
  struct FrameCaptureRange {
    uint32_t start, length, seq;
  };
  // What the resolves of the frame before the capture and of the captured frame wrote.
  std::vector<FrameCaptureRange> frame_capture_armed_ranges_;
  std::vector<FrameCaptureRange> frame_capture_frame_ranges_;
  uint32_t frame_capture_textures_skipped_ = 0;
  void WriteFrameCaptureHeader(FILE* f);
  // Texture -> the resolve sequence number it was captured after (once per texture and resolve).
  std::unordered_map<const void*, int64_t> frame_capture_textures_done_;
  uint32_t frame_capture_texture_count_ = 0;
  uint64_t frame_capture_texture_bytes_ = 0;

  bool submission_open_ = false;
  // Draws recorded since the current submission began (vulkan_submit_before_wait).
  uint32_t submission_draws_ = 0;
  uint64_t wait_submissions_ = 0;
  uint64_t submission_completed_ = 0;
  // In case vkQueueSubmit fails after something like a successful
  // vkQueueBindSparse, to wait correctly on the next attempt.
  std::vector<VkSemaphore> current_submission_wait_semaphores_;
  std::vector<VkPipelineStageFlags> current_submission_wait_stage_masks_;
  std::vector<VkFence> submissions_in_flight_fences_;
  std::deque<std::pair<uint64_t, VkSemaphore>> submissions_in_flight_semaphores_;

  static constexpr uint32_t kMaxFramesInFlight = 3;
  bool frame_open_ = false;
  // Tracks whether any draw in the current frame used an async placeholder
  // graphics pipeline and may have produced incomplete output.
  bool frame_used_async_placeholder_pipeline_ = false;
  // Pipeline / shader work at draw time, logged every 10 s ("Pipelines (10 s)").
  // Dynamic state last set for pipelines linked from libraries (unknown after a pipeline with
  // static state is bound or a new command buffer starts).
  VulkanPipelineCache::PipelineDynamicState current_library_dynamic_state_;
  bool current_library_dynamic_state_valid_ = false;
  void ApplyLibraryPipelineDynamicState(const VulkanPipelineCache::PipelineDynamicState& state);
  // Pipeline library layouts by the stage's texture set layout (shared by every pair).
  std::recursive_mutex pipeline_layouts_mutex_;
  std::unordered_map<VkDescriptorSetLayout, VkPipelineLayout> library_layouts_vertex_;
  std::unordered_map<VkDescriptorSetLayout, VkPipelineLayout> library_layouts_pixel_;
  VkPipelineLayout GetLibraryLayout(bool vertex_stage, VkDescriptorSetLayout textures_layout);

 public:
  // Thread-safe: the layout of a pre-rasterization (vertex_stage) or fragment pipeline library
  // whose shader uses the given numbers of textures and samplers.
  VkPipelineLayout GetLibraryLayoutForTextures(bool vertex_stage, size_t texture_count,
                                               size_t sampler_count);

 private:
  uint32_t counter_draws_skipped_pending_ = 0;
  uint32_t counter_frames_held_ = 0;
  uint32_t counter_frames_presented_ = 0;
  uint64_t pipeline_counters_last_log_ns_ = 0;
  void LogPipelineCountersIfDue();
  // Guest frame index, since some transient resources can be reused across
  // submissions. Values updated in the beginning of a frame.
  uint64_t frame_current_ = 1;
  uint64_t frame_completed_ = 0;
  // Submission indices of frames that have already been submitted.
  uint64_t closed_frame_submissions_[kMaxFramesInFlight] = {};

  // <Submission where last used, resource>, sorted by the submission number.
  std::deque<std::pair<uint64_t, VkDeviceMemory>> destroy_memory_;
  std::deque<std::pair<uint64_t, VkBuffer>> destroy_buffers_;
  std::deque<std::pair<uint64_t, VkImageView>> destroy_image_views_;
  std::deque<std::pair<uint64_t, VkImage>> destroy_images_;
  std::deque<std::pair<uint64_t, VkFramebuffer>> destroy_framebuffers_;

  std::vector<CommandBuffer> command_buffers_writable_;
  std::deque<std::pair<uint64_t, CommandBuffer>> command_buffers_submitted_;
  DeferredCommandBuffer deferred_command_buffer_;

  std::vector<VkSparseMemoryBind> sparse_memory_binds_;
  std::vector<SparseBufferBind> sparse_buffer_binds_;
  // SparseBufferBind converted to VkSparseBufferMemoryBindInfo to this buffer
  // on submission (because pBinds should point to a place in std::vector, but
  // it may be reallocated).
  std::vector<VkSparseBufferMemoryBindInfo> sparse_buffer_bind_infos_temp_;
  VkPipelineStageFlags sparse_bind_wait_stage_mask_ = 0;

  // Temporary storage with reusable memory for creating descriptor set layouts.
  std::vector<VkDescriptorSetLayoutBinding> descriptor_set_layout_bindings_;
  // Temporary storage with reusable memory for writing image and sampler
  // descriptors.
  std::vector<VkDescriptorImageInfo> descriptor_write_image_info_;
  // vulkan_reuse_texture_descriptor_sets: the texture/sampler descriptor set written last for each stage (0 vertex,
  // 1 pixel) in this frame and what it holds, so a draw with the same bindings reuses it.
  struct TextureSetCache {
    uint64_t frame = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    uint32_t texture_count = 0;
    std::vector<VkDescriptorImageInfo> infos;
  };
  TextureSetCache texture_set_cache_[2];
  uint64_t texture_set_cache_hits_ = 0;
  uint64_t texture_set_cache_misses_ = 0;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> uniform_buffer_pool_;

  // Descriptor set layouts used by different shaders.
  VkDescriptorSetLayout descriptor_set_layout_empty_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_constants_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, size_t(SingleTransientDescriptorLayout::kCount)>
      descriptor_set_layouts_single_transient_{};
  VkDescriptorSetLayout descriptor_set_layout_shared_memory_and_edram_ = VK_NULL_HANDLE;

  // Descriptor set layouts are referenced by pipeline_layouts_.
  std::unordered_map<TextureDescriptorSetLayoutKey, VkDescriptorSetLayout,
                     TextureDescriptorSetLayoutKey::Hasher>
      descriptor_set_layouts_textures_;
  // Pipeline layouts are referenced by VulkanPipelineCache.
  std::unordered_map<PipelineLayoutKey, PipelineLayout, PipelineLayoutKey::Hasher>
      pipeline_layouts_;

  // No specific reason for 32768, just the "too much" descriptor count from
  // Direct3D 12 PIX warnings.
  static constexpr uint32_t kLinkedTypeDescriptorPoolSetCount = 32768;
  static const VkDescriptorPoolSize kDescriptorPoolSizeUniformBuffer;
  static const VkDescriptorPoolSize kDescriptorPoolSizeStorageBuffer;
  // vulkan_dynamic_constant_offsets: one set per uniform buffer pool page, so few sets are ever needed.
  static constexpr uint32_t kDynamicConstantsDescriptorPoolSetCount = 64;
  static const VkDescriptorPoolSize kDescriptorPoolSizeUniformBufferDynamic;
  static const VkDescriptorPoolSize kDescriptorPoolSizeTextures[2];
  ui::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_uniform_buffer_;
  ui::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_storage_buffer_;
  std::deque<UsedSingleTransientDescriptor> single_transient_descriptors_used_;
  std::array<std::vector<VkDescriptorSet>, size_t(SingleTransientDescriptorLayout::kCount)>
      single_transient_descriptors_free_;
  // <Usage frame, set>.
  std::deque<std::pair<uint64_t, VkDescriptorSet>> constants_transient_descriptors_used_;
  std::vector<VkDescriptorSet> constants_transient_descriptors_free_;
  // vulkan_dynamic_constant_offsets (read once in SetupContext): the constants descriptor set layout has
  // UNIFORM_BUFFER_DYNAMIC bindings, and there is one long-lived descriptor set per uniform_buffer_pool_ page
  // (<page buffer, set>, all five bindings at offset 0 of that buffer with the largest range) instead of one
  // transient set per constants change.
  bool constants_dynamic_offsets_ = false;
  // False with vulkan_dynamic_constant_offsets_per_change_sets (diagnostic): dynamic layout, a set per change.
  bool constants_dynamic_per_page_sets_ = false;
  uint32_t constants_dynamic_binds_logged_ = 0;
  size_t constants_dynamic_page_size_ = 0;
  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_uniform_buffer_dynamic_;
  std::vector<std::pair<VkBuffer, VkDescriptorSet>> constants_dynamic_descriptor_sets_;

  ui::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_textures_;
  std::deque<UsedTextureTransientDescriptorSet> texture_transient_descriptor_sets_used_;
  std::unordered_map<TextureDescriptorSetLayoutKey, std::vector<VkDescriptorSet>,
                     TextureDescriptorSetLayoutKey::Hasher>
      texture_transient_descriptor_sets_free_;

  std::unique_ptr<VulkanSharedMemory> shared_memory_;

  std::unique_ptr<VulkanPrimitiveProcessor> primitive_processor_;

  std::unique_ptr<VulkanRenderTargetCache> render_target_cache_;

  std::unique_ptr<VulkanPipelineCache> pipeline_cache_;

  std::unique_ptr<VulkanTextureCache> texture_cache_;

  VkDescriptorPool shared_memory_and_edram_descriptor_pool_ = VK_NULL_HANDLE;
  // R32_UINT view of the whole shared memory for shader reads (vulkan_shared_memory_texel_buffer).
  VkBufferView shared_memory_texel_buffer_view_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_and_edram_descriptor_set_;

  // Bytes 0x0...0x3FF - 256-entry gamma ramp table with B10G10R10X2 data (read
  // as R10G10B10X2 with swizzle).
  // Bytes 0x400...0x9FF - 128-entry PWL R16G16 gamma ramp (R - base, G - delta,
  // low 6 bits of each are zero, 3 elements per entry).
  // kMaxFramesInFlight pairs of gamma ramps if in host-visible memory and
  // uploaded directly, one otherwise.
  VkDeviceMemory gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_buffer_ = VK_NULL_HANDLE;
  // kMaxFramesInFlight pairs, only when the gamma ramp buffer is not
  // host-visible.
  VkDeviceMemory gamma_ramp_upload_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_upload_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize gamma_ramp_upload_memory_size_;
  uint32_t gamma_ramp_upload_memory_type_;
  // Mapping of either gamma_ramp_buffer_memory_ (if it's host-visible) or
  // gamma_ramp_upload_buffer_memory_ (otherwise).
  void* gamma_ramp_upload_mapping_;
  std::array<VkBufferView, 2 * kMaxFramesInFlight> gamma_ramp_buffer_views_{};
  // UINT32_MAX if outdated.
  uint32_t gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  uint32_t gamma_ramp_pwl_current_frame_ = UINT32_MAX;

  VkDescriptorSetLayout swap_descriptor_set_layout_sampled_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_combined_image_sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_storage_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_uniform_texel_buffer_ = VK_NULL_HANDLE;

  // Descriptor pool for allocating descriptors needed for presentation, such as
  // the destination images and the gamma ramps.
  VkDescriptorPool swap_descriptor_pool_ = VK_NULL_HANDLE;
  // Interleaved 256-entry table and PWL texel buffer descriptors.
  // kMaxFramesInFlight pairs of gamma ramps if in host-visible memory and
  // uploaded directly, one otherwise.
  std::array<VkDescriptorSet, 2 * kMaxFramesInFlight> swap_descriptors_gamma_ramp_;
  // Sampled images.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_source_;
  // Combined image sampler descriptors for FXAA.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_fxaa_source_;
  // Storage image descriptors for the apply-gamma compute pass destination.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_destination_storage_;
  // Separate storage image descriptors for the FXAA compute destination.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_fxaa_destination_storage_;

  VkSampler swap_sampler_linear_clamp_ = VK_NULL_HANDLE;

  VkPipelineLayout swap_apply_gamma_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout swap_apply_gamma_compute_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout swap_fxaa_pipeline_layout_ = VK_NULL_HANDLE;
  // Has no dependencies on specific pipeline stages on both ends to simplify
  // use in different scenarios with different pipelines - use explicit barriers
  // for synchronization.
  VkRenderPass swap_apply_gamma_render_pass_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_fxaa_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_fxaa_extreme_pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout resolve_downscale_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline resolve_downscale_pipeline_ = VK_NULL_HANDLE;
  VkBuffer resolve_downscale_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
  uint32_t resolve_downscale_buffer_size_ = 0;

  VkImage swap_fxaa_source_image_ = VK_NULL_HANDLE;
  VkDeviceMemory swap_fxaa_source_image_memory_ = VK_NULL_HANDLE;
  VkImageView swap_fxaa_source_image_view_ = VK_NULL_HANDLE;
  uint32_t swap_fxaa_source_image_width_ = 0;
  uint32_t swap_fxaa_source_image_height_ = 0;
  uint64_t swap_fxaa_source_image_submission_ = 0;
  VkPipelineStageFlags swap_fxaa_source_stage_mask_ = 0;
  VkAccessFlags swap_fxaa_source_access_mask_ = 0;
  VkImageLayout swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

  std::array<SwapFramebuffer, ui::vulkan::VulkanPresenter::kMaxActiveGuestOutputImageVersions>
      swap_framebuffers_;

  // Pending pipeline barriers.
  std::vector<VkBufferMemoryBarrier> pending_barriers_buffer_memory_barriers_;
  std::vector<VkImageMemoryBarrier> pending_barriers_image_memory_barriers_;
  struct PendingBarrier {
    VkPipelineStageFlags src_stage_mask = 0;
    VkPipelineStageFlags dst_stage_mask = 0;
    size_t buffer_memory_barriers_offset = 0;
    size_t image_memory_barriers_offset = 0;
  };
  std::vector<PendingBarrier> pending_barriers_;
  PendingBarrier current_pending_barrier_;

  // GPU-local scratch buffer.
  static constexpr VkDeviceSize kScratchBufferSizeIncrement = 16 * 1024 * 1024;
  VkDeviceMemory scratch_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer scratch_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize scratch_buffer_size_ = 0;
  VkPipelineStageFlags scratch_buffer_last_stage_mask_ = 0;
  VkAccessFlags scratch_buffer_last_access_mask_ = 0;
  uint64_t scratch_buffer_last_usage_submission_ = 0;
  bool scratch_buffer_used_ = false;

  static constexpr uint32_t kMaxOcclusionQueries = 8192;
  VkQueryPool occlusion_query_pool_ = VK_NULL_HANDLE;
  VkBuffer occlusion_query_readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory occlusion_query_readback_memory_ = VK_NULL_HANDLE;
  uint32_t occlusion_query_readback_memory_type_ = UINT32_MAX;
  VkDeviceSize occlusion_query_readback_memory_size_ = 0;
  uint8_t* occlusion_query_readback_mapping_ = nullptr;
  uint32_t occlusion_query_cursor_ = 0;
  bool occlusion_query_resources_available_ = false;
  struct ActiveOcclusionQuery {
    uint32_t sample_count_address = 0;
    uint32_t host_index = UINT32_MAX;
    bool valid = false;
  } active_occlusion_query_;
  // vulkan_skip_no_effect_draws: draws left out since the last GPU timing line.
  uint64_t no_effect_draws_skipped_ = 0;
  struct VertexBufferState {
    uint32_t address = UINT32_MAX;
    uint32_t size = UINT32_MAX;
  };
  std::array<VertexBufferState, 96> vertex_buffer_states_{};
  uint64_t vertex_buffers_in_sync_[2] = {};
  std::unordered_map<uint64_t, ReadbackBuffer> readback_buffers_;
  std::unordered_map<uint64_t, ReadbackBuffer> memexport_readback_buffers_;

  // The current dynamic state of the graphics pipeline bind point. Note that
  // binding any pipeline to the bind point with static state (even if it's
  // unused, like depth bias being disabled, but the values themselves still not
  // declared as dynamic in the pipeline) invalidates such dynamic state.
  VkViewport dynamic_viewport_;
  VkRect2D dynamic_scissor_;
  // gpu_debug_zero_scissor_vertex_shader matched the current draw (diagnostics).
  bool debug_zero_scissor_ = false;
  // Submissions ended by vulkan_submit_every_draws (statistics).
  uint64_t draw_count_submissions_ = 0;
  // Dynamic fixed-function depth bias, blend constants, stencil state are
  // applicable only to the render target implementations where they are
  // actually involved.
  float dynamic_depth_bias_constant_factor_;
  float dynamic_depth_bias_slope_factor_;
  // gpu_float24_absolute_depth_bias: the current draw's constant polygon offset is applied in the
  // vertex shader (ndc_offset.z), so the fixed-function constant factor is 0.
  bool float24_depth_bias_in_vertex_shader_ = false;
  // Eco mode (gpu_eco_shading_rate): the coarse rate for 3D scene draws, width | height << 4 (0 =
  // off), re-read at every command buffer start; the rate last set in this command buffer and
  // whether it is still valid (binding a pipeline without the dynamic state invalidates it).
  uint32_t eco_shading_rate_ = 0;
  uint32_t current_fragment_shading_rate_ = 0;
  bool fragment_shading_rate_valid_ = false;
  float dynamic_blend_constants_[4];
  // The stencil values are pre-initialized (to D3D11_DEFAULT_STENCIL_*, and the
  // initial values for front and back are the same for portability subset
  // safety) because they're updated conditionally to avoid changing the back
  // face values when stencil is disabled and the primitive type is changed
  // between polygonal and non-polygonal.
  uint32_t dynamic_stencil_compare_mask_front_ = UINT8_MAX;
  uint32_t dynamic_stencil_compare_mask_back_ = UINT8_MAX;
  uint32_t dynamic_stencil_write_mask_front_ = UINT8_MAX;
  uint32_t dynamic_stencil_write_mask_back_ = UINT8_MAX;
  uint32_t dynamic_stencil_reference_front_ = 0;
  uint32_t dynamic_stencil_reference_back_ = 0;
  bool dynamic_viewport_update_needed_;
  bool dynamic_scissor_update_needed_;
  bool dynamic_depth_bias_update_needed_;
  bool dynamic_blend_constants_update_needed_;
  bool dynamic_stencil_compare_mask_front_update_needed_;
  bool dynamic_stencil_compare_mask_back_update_needed_;
  bool dynamic_stencil_write_mask_front_update_needed_;
  bool dynamic_stencil_write_mask_back_update_needed_;
  bool dynamic_stencil_reference_front_update_needed_;
  bool dynamic_stencil_reference_back_update_needed_;

  // Currently used samplers.
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>> current_samplers_vertex_;
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>> current_samplers_pixel_;

  // Cache render pass currently started in the command buffer with the
  // framebuffer. For dynamic rendering, current_render_pass_ is VK_NULL_HANDLE
  // but in_render_pass_ is true.
  VkRenderPass current_render_pass_;
  const VulkanRenderTargetCache::Framebuffer* current_framebuffer_;
  bool in_render_pass_ = false;

  // Area written by the commands in the current render target cache render
  // pass instance, see ExtendRenderPassArea.
  struct RenderPassArea {
    // Whether a render target cache render pass instance is being recorded.
    bool active = false;
    // vulkan_render_area_restrict at the beginning of the instance.
    bool restrict_enabled = false;
    // Location of the render area of the begin command in the deferred command
    // buffer.
    size_t begin_render_area_offset = SIZE_MAX;
    uint32_t framebuffer_width = 0;
    uint32_t framebuffer_height = 0;
    // Union of the written rectangles, clamped to the framebuffer, empty if
    // x0 >= x1 or y0 >= y1.
    uint32_t x0 = UINT32_MAX;
    uint32_t y0 = UINT32_MAX;
    uint32_t x1 = 0;
    uint32_t y1 = 0;
  };
  // Call after recording the begin command of a render target cache render
  // pass instance.
  void RenderPassAreaBegin(const VkExtent2D& framebuffer_extent);
  // Call before recording the end command of a render target cache render pass
  // instance, while the begin command can still be patched.
  void RenderPassAreaEnd();
  RenderPassArea render_pass_area_;

  // Currently bound graphics pipeline, either from the pipeline cache (with
  // potentially deferred creation - current_external_graphics_pipeline_ is
  // VK_NULL_HANDLE in this case) or a non-Xenos one
  // (current_guest_graphics_pipeline_ is VK_NULL_HANDLE in this case).
  // TODO(Triang3l): Change to a deferred compilation handle.
  VkPipeline current_guest_graphics_pipeline_;
  VkPipeline current_external_graphics_pipeline_;
  VkPipeline current_external_compute_pipeline_;

  // Pipeline layout of the current guest graphics pipeline.
  const PipelineLayout* current_guest_graphics_pipeline_layout_;
  VkDescriptorBufferInfo
      current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferCount];
  // Whether up-to-date data has been written to constant (uniform) buffers, and
  // the buffer infos in current_constant_buffer_infos_ point to them.
  uint32_t current_constant_buffers_up_to_date_;
  // vulkan_dynamic_constant_offsets: offsets of current_constant_buffer_infos_ in their (common) pool page.
  uint32_t current_constant_buffer_dynamic_offsets_[SpirvShaderTranslator::kConstantBufferCount] = {};
  VkDescriptorSet current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetCount];
  // Whether descriptor sets in current_graphics_descriptor_sets_ point to
  // up-to-date data.
  uint32_t current_graphics_descriptor_set_values_up_to_date_;
  // Whether the descriptor sets currently bound to the command buffer - only
  // low bits for the descriptor set layouts that remained the same are kept
  // when changing the pipeline layout. May be out of sync with
  // current_graphics_descriptor_set_values_up_to_date_, but should be ensured
  // to be a subset of it at some point when it becomes important; bits for
  // non-existent descriptor set layouts may also be set, but need to be ignored
  // when they start to matter.
  uint32_t current_graphics_descriptor_sets_bound_up_to_date_;
  static_assert(SpirvShaderTranslator::kDescriptorSetCount <=
                    sizeof(current_graphics_descriptor_set_values_up_to_date_) * CHAR_BIT,
                "Bit fields storing descriptor set validity must be large enough");
  static_assert(SpirvShaderTranslator::kDescriptorSetCount <=
                    sizeof(current_graphics_descriptor_sets_bound_up_to_date_) * CHAR_BIT,
                "Bit fields storing descriptor set validity must be large enough");

  // Float constant usage masks of the last draw call.
  uint64_t current_float_constant_map_vertex_[4];
  uint64_t current_float_constant_map_pixel_[4];

  // System shader constants.
  SpirvShaderTranslator::SystemConstants system_constants_;

  // Temporary storage for memexport stream constants used in the draw.
  std::vector<draw_util::MemExportRange> memexport_ranges_;

  // vulkan_gpu_timing.
  static constexpr uint32_t kGpuTimingMaxTimestamps = 4096;
  struct GpuTimingSubmission {
    VkQueryPool pool = VK_NULL_HANDLE;
    uint32_t timestamp_count = 0;
    bool closed = false;
    // Category of the segment that starts at each timestamp.
    std::vector<uint8_t> categories;
    // Render pass signature of draw and transfer segments (0 for others).
    std::vector<uint64_t> tags;
    // vulkan_gpu_trace_frame: timestamp query written after each draw of the
    // traced frame, and the index of the draw in gpu_trace_draws_.
    std::vector<std::pair<uint32_t, uint32_t>> draw_queries;
    // Guest frame (swap count) the submission belongs to.
    uint64_t frame = 0;
  };
  // vulkan_slow_frame_ms on the GPU side: the GPU time of each frame, from its submissions.
  uint64_t gpu_timing_frame_index_ = 0;
  uint64_t gpu_frame_accum_frame_ = 0;
  double gpu_frame_accum_ns_[size_t(GpuTimingCategory::kCount)] = {};
  // The last frame whose GPU time is complete (vulkan_gpu_timing): its index and ms, for the slow-frame line.
  uint64_t gpu_last_frame_index_ = 0;
  double gpu_last_frame_ms_ = 0.0;
  void GpuTimingFlushFrame();
  void GpuTimingWriteTimestamp(GpuTimingCategory category);
  void GpuTimingBeginSubmission();
  void GpuTimingEndSubmission();
  void GpuTimingReadCompleted();
  void GpuTimingOnSwap();
  void GpuTimingLogPasses(double frames);
  static std::string GpuTimingDescribeTag(uint64_t tag);
  void GpuTimingShutdown();
  bool gpu_timing_enabled_ = false;
  // vulkan_lr_skip_battle_grey: a crossfade draw happened in this frame.
  bool lr_crossfade_this_frame_ = false;
  // 0 - not checked yet, 1 - supported, -1 - unsupported.
  int gpu_timing_support_ = 0;
  double gpu_timing_period_ns_ = 1.0;
  uint64_t gpu_timing_valid_mask_ = UINT64_MAX;
  bool gpu_timing_in_resolve_ = false;
  GpuTimingCategory gpu_timing_segment_category_ = GpuTimingCategory::kCount;
  uint64_t gpu_timing_pass_tag_ = 0;
  uint64_t gpu_timing_segment_tag_ = 0;
  uint64_t gpu_timing_op_tag_ = 0;
  struct GpuTimingTagStats {
    uint64_t passes = 0;
    uint64_t draws = 0;
    double ns = 0.0;
  };
  std::unordered_map<uint64_t, GpuTimingTagStats> gpu_timing_tag_stats_;
  std::unordered_map<uint64_t, GpuTimingTagStats> gpu_timing_op_stats_;
  std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> gpu_timing_transfer_pairs_;
  enum PassEndReason : uint32_t {
    kPassEndOther,
    kPassEndFramebuffer,
    kPassEndForced,
    kPassEndBarrierRead,
    kPassEndBarrierAttachment,
    kPassEndBarrierGeneral,
    kPassEndBarrierTransfer,
    kPassEndBarrierBuffer,
    kPassEndCount,
  };
  PassEndReason pass_end_reason_ = kPassEndOther;
  void GpuTimingNotePassEnd(PassEndReason reason) {
    ++gpu_timing_counters_.pass_end_reasons[reason];
    if (gpu_trace_active_) {
      gpu_trace_ += ")";
      gpu_trace_ += std::to_string(gpu_trace_pass_draws_);
      gpu_trace_ += "ofFRagtb"[reason];
      gpu_trace_ += ' ';
    }
  }
  uint32_t gpu_trace_frames_left_ = 0;
  uint32_t gpu_trace_windows_ = 0;
  bool gpu_trace_armed_ = false;
  struct GpuTraceDraw {
    uint64_t vertex_shader_hash;
    uint64_t pixel_shader_hash;
    uint64_t pass_tag;
    uint32_t primitive_type;
    uint32_t index_count;
  };
  std::vector<GpuTraceDraw> gpu_trace_draws_;
  // Draw index, GPU ns from the previous timestamp to the end of the draw.
  std::vector<std::pair<uint32_t, double>> gpu_trace_draw_ns_;
  void GpuTraceDrawTimestamp(uint64_t vertex_shader_hash, uint64_t pixel_shader_hash,
                             uint32_t primitive_type, uint32_t index_count);
  void GpuTraceLogDraws();
  bool gpu_trace_active_ = false;
  uint32_t gpu_trace_pass_draws_ = 0;
  std::string gpu_trace_;
  GpuTimingSubmission gpu_timing_current_;
  std::vector<GpuTimingSubmission> gpu_timing_free_;
  std::deque<std::pair<uint64_t, GpuTimingSubmission>> gpu_timing_submitted_;
  std::vector<uint64_t> gpu_timing_results_;
  // Accumulated over the current logging window.
  double gpu_timing_category_ns_[size_t(GpuTimingCategory::kCount)] = {};
  uint64_t gpu_timing_submissions_measured_ = 0;
  uint64_t gpu_timing_overflows_ = 0;
  uint64_t gpu_timing_frames_ = 0;
  // GPU clock (Adreno kgsl, sampled once per frame) over the window: GPU time
  // alone hides work changes when the governor scales the clock.
  double gpu_timing_clock_mhz_sum_ = 0.0;
  uint64_t gpu_timing_clock_samples_ = 0;
  uint64_t gpu_timing_window_start_ns_ = 0;
  GpuTimingCounters gpu_timing_counters_;
};

}  // namespace rex::graphics::vulkan
