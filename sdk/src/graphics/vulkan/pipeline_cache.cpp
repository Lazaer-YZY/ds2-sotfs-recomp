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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/thread.h>
#include <rex/memory.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/flags.h>
#include <rex/cvar.h>

REXCVAR_DECLARE(std::string, vulkan_fragment_shading_rate);
REXCVAR_DECLARE(bool, vulkan_dynamic_fragment_shading_rate);
#include <rex/graphics/pipeline_util.h>
#include <rex/graphics/pipeline/shader/spirv_builder.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/pipeline_cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/types.h>
#include <rex/platform.h>
#include <rex/ui/vulkan/util.h>

#if REX_PLATFORM_WIN32
#include <io.h>
#else
#include <unistd.h>
#if defined(__ANDROID__)
#include <sched.h>
#include <sys/resource.h>
#endif
#endif

REXCVAR_DECLARE(bool, spirv_texture_sign_branch);
REXCVAR_DECLARE(bool, vulkan_shader_xenos_multiply);
REXCVAR_DECLARE(bool, vulkan_shader_xenos_multiply_vertex);
REXCVAR_DECLARE(bool, vulkan_texture_fetch_implicit_lod);
REXCVAR_DECLARE(bool, vulkan_shared_memory_texel_buffer);
REXCVAR_DECLARE(bool, spirv_structured_forward_jumps);

// Test: the game's user clip planes become gl_ClipDistance outputs (a vertex shader variant); on Adreno a clip
// distance can send every primitive through the clipper instead of the guard band. Dropping them shows their cost
// (wrong only where a plane really clips something, e.g. a reflection pass). New shader variants on change.
REXCVAR_DEFINE_BOOL(gpu_debug_drop_user_clip_planes, false, "GPU/Vulkan",
                    "Test only: ignore the game's user clip planes (no clip distances in vertex shaders)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_pipeline_libraries, true, "GPU/Vulkan",
                    "Link guest pipelines from per-stage libraries (VK_EXT_graphics_pipeline_"
                    "library with fast linking, extended dynamic state 1/2/3, dynamic rendering): "
                    "each shader is compiled once into a library that can be built ahead of time, "
                    "and new state combinations only need a fast link.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vulkan_async_shader_translation, true, "GPU/Vulkan",
                    "Translate guest shaders to SPIR-V on the pipeline creation threads instead "
                    "of the GPU thread when the pipeline is created asynchronously (a "
                    "translation takes 10-25 ms on a phone; the draws wait for the pipeline "
                    "either way).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(vulkan_pipeline_compile_never_blocks, true, "GPU/Vulkan",
                    "With async_shader_compilation, a pipeline missing at draw time is only "
                    "created by the background creation threads: its draws are skipped until it "
                    "is ready (no placeholder pipeline compiled on the GPU thread, also for draws "
                    "without a pixel shader, and no wait for the creation threads at the end of "
                    "each submission).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_storage_translate_all, false, "GPU/Vulkan",
                    "At startup, translate every shader in the shader storage with its default "
                    "modification (vertex shaders exporting every interpolator they write, pixel "
                    "shaders reading all 16) and log how many failed: the translation report for "
                    "an offline-extracted shader set (tools/lr_shader_scan.py).");

REXCVAR_DEFINE_INT32(vulkan_storage_translation_threads, -1, "GPU/Vulkan",
                     "Threads translating the stored shaders at startup: -1 = as many as the "
                     "pipeline creation threads, 1 = on the GPU thread only.")
    .range(-1, 64);

REXCVAR_DEFINE_INT32(
    vulkan_pipeline_creation_threads, -1, "GPU/Vulkan",
    "Number of pipeline creation threads for Vulkan async pipeline creation (-1 for auto)")
    .range(-1, 32)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vulkan_pipeline_creation_little_cores, false, "GPU/Vulkan",
                    "Android: run the pipeline creation threads (compiles during gameplay) on the "
                    "slowest CPU cluster only, so first-visit compiles don't compete with the "
                    "game's threads on the fast cores (the boot precompile keeps the fast cores)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(vulkan_precompile_in_game, 2, "GPU/Vulkan",
                     "Android: when gameplay starts before the boot pipeline library precompile "
                     "has finished: 0 = keep compiling on the fast cores, 1 = finish on the "
                     "slowest CPU cluster, 2 = stop (it resumes at the title on the next launch)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(vulkan_precompile_vertex_libraries, true, "GPU/Vulkan",
                    "The boot precompile also builds the pre-rasterization (vertex shader) "
                    "libraries: the variants used in play (from the stored pipelines) after the "
                    "used pixel shaders, the canonical variant of every other stored vertex "
                    "shader at the end. A pipeline state seen for the first time then links in "
                    "~1-3 ms instead of compiling its vertex library on the spot (~130 ms)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(vulkan_precompile_threads, 0, "GPU/Vulkan",
                     "Threads for the boot pipeline library precompile (on Android: on the faster "
                     "CPU clusters); 0 = min(4, logical processors / 2)")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(vulkan_precompile_little_threads, 0, "GPU/Vulkan",
                     "Android: extra boot precompile threads on the slowest CPU cluster (idle at "
                     "the title)")
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(vulkan_tessellation_wireframe, false, "GPU/Vulkan",
                    "Render tessellation as wireframe")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(vulkan_pipeline_ir_dump_ps, "", "GPU/Vulkan",
                      "Diagnostics (with vulkan_pipeline_statistics and monolithic pipelines): "
                      "comma-separated pixel shader ucode hashes (16 hex digits) whose pipelines are "
                      "created with the driver's internal representations (e.g. ir3 disassembly) "
                      "captured and logged")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::graphics::vulkan {

namespace {

// fflush only hands the data to the OS, which already survives the process being killed (the
// Android app is always ended with a force-stop); the sync also makes it survive a power loss.
bool FlushAndSyncStorageFile(FILE* file) {
  if (fflush(file)) {
    return false;
  }
#if REX_PLATFORM_WIN32
  return _commit(_fileno(file)) == 0;
#else
  return fsync(fileno(file)) == 0;
#endif
}

// Never 0, which means "never" in the driver pipeline cache save timestamps.
// Opens a storage file for appending. A file the app can read but not write (copied in by adb or
// a file manager, so owned by another user) is rewritten as a copy the app owns, through a
// temporary file and a rename (which needs write access only to the folder, which the app has),
// instead of disabling the storage for the whole run.
FILE* OpenStorageFileForAppend(const std::filesystem::path& path) {
  if (FILE* file = rex::filesystem::OpenFile(path, "a+b")) {
    return file;
  }
  std::error_code error_code;
  if (!std::filesystem::exists(path, error_code)) {
    return nullptr;
  }
  std::vector<uint8_t> data;
  if (FILE* source = rex::filesystem::OpenFile(path, "rb")) {
    rex::filesystem::Seek(source, 0, SEEK_END);
    int64_t size = rex::filesystem::Tell(source);
    rex::filesystem::Seek(source, 0, SEEK_SET);
    if (size > 0 && size < (int64_t(1) << 30)) {
      data.resize(size_t(size));
      if (fread(data.data(), 1, data.size(), source) != data.size()) {
        data.clear();
      }
    }
    fclose(source);
  }
  std::filesystem::path own_path = path;
  own_path += ".own";
  bool written = false;
  if (FILE* own = rex::filesystem::OpenFile(own_path, "wb")) {
    written = data.empty() || fwrite(data.data(), 1, data.size(), own) == data.size();
    written = fclose(own) == 0 && written;
  }
  if (written) {
    std::filesystem::rename(own_path, path, error_code);
  }
  if (!written || error_code) {
    std::filesystem::remove(own_path, error_code);
    return nullptr;
  }
  REXGPU_WARN("VulkanPipelineCache: {} was not writable by the app (copied in by another user?), "
              "rewrote it as the app's own copy ({} bytes)",
              rex::path_to_utf8(path), data.size());
  return rex::filesystem::OpenFile(path, "a+b");
}

uint64_t GetDriverPipelineCacheClockMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count()) +
         1;
}

constexpr std::string_view kPlaceholderPixelShaderSource = R"(#version 460
layout(location = 0) out vec4 oC0;
void main() {
  // Avoid full-screen black flashes while async pipelines are warming up.
  discard;
}
)";

constexpr bool IsTriangleDomainHostVertexShaderType(
    Shader::HostVertexShaderType host_vertex_shader_type) {
  return host_vertex_shader_type == Shader::HostVertexShaderType::kTriangleDomainCPIndexed ||
         host_vertex_shader_type == Shader::HostVertexShaderType::kTriangleDomainPatchIndexed;
}

constexpr bool IsQuadDomainHostVertexShaderType(
    Shader::HostVertexShaderType host_vertex_shader_type) {
  return host_vertex_shader_type == Shader::HostVertexShaderType::kQuadDomainCPIndexed ||
         host_vertex_shader_type == Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
}

constexpr bool IsPatchIndexedHostVertexShaderType(
    Shader::HostVertexShaderType host_vertex_shader_type) {
  return host_vertex_shader_type == Shader::HostVertexShaderType::kTriangleDomainPatchIndexed ||
         host_vertex_shader_type == Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
}

std::string GetTessellationSystemConstantsBlockGlsl() {
  std::string source;
  source += "layout(set = 0, binding = 0, std140) uniform XeSystemConstants {\n";
  source += fmt::format("  layout(offset = {}) uint xe_vertex_index_endian;\n",
                        offsetof(SpirvShaderTranslator::SystemConstants, vertex_index_endian));
  source += fmt::format("  layout(offset = {}) int xe_vertex_base_index;\n",
                        offsetof(SpirvShaderTranslator::SystemConstants, vertex_base_index));
  source += fmt::format("  layout(offset = {}) uint xe_vertex_index_min;\n",
                        offsetof(SpirvShaderTranslator::SystemConstants, vertex_index_min));
  source += fmt::format("  layout(offset = {}) uint xe_vertex_index_max;\n",
                        offsetof(SpirvShaderTranslator::SystemConstants, vertex_index_max));
  source +=
      fmt::format("  layout(offset = {}) float xe_tessellation_factor_range_min;\n",
                  offsetof(SpirvShaderTranslator::SystemConstants, tessellation_factor_range_min));
  source +=
      fmt::format("  layout(offset = {}) float xe_tessellation_factor_range_max;\n",
                  offsetof(SpirvShaderTranslator::SystemConstants, tessellation_factor_range_max));
  source += "} xe_system_cbuffer;\n";
  return source;
}

std::string GetTessellationIndexedVertexShaderGlsl() {
  std::string source;
  source += "#version 450\n";
  source += GetTessellationSystemConstantsBlockGlsl();
  source += R"(
layout(location = 0) out float xe_out_value;

uint xe_swap_8_in_16(uint value) {
  return ((value << 8u) & 0xFF00FF00u) | ((value >> 8u) & 0x00FF00FFu);
}

uint xe_swap_16_in_32(uint value) {
  return ((value & 0x0000FFFFu) << 16u) | (value >> 16u);
}

void main() {
  uint value = uint(gl_VertexIndex);
  uint endian = xe_system_cbuffer.xe_vertex_index_endian;
  if (endian == 1u || endian == 2u || endian == 3u) {
    value = xe_swap_8_in_16(value);
  }
  if (endian == 2u || endian == 3u) {
    value = xe_swap_16_in_32(value);
  }
  value = uint(int(value) + xe_system_cbuffer.xe_vertex_base_index);
  value &= 0x00FFFFFFu;
  value = max(value, xe_system_cbuffer.xe_vertex_index_min);
  value = min(value, xe_system_cbuffer.xe_vertex_index_max);
  xe_out_value = float(value);
}
)";
  return source;
}

std::string GetTessellationAdaptiveVertexShaderGlsl() {
  std::string source;
  source += "#version 450\n";
  source += GetTessellationSystemConstantsBlockGlsl();
  source += R"(
layout(location = 0) out float xe_out_value;

uint xe_swap_8_in_16(uint value) {
  return ((value << 8u) & 0xFF00FF00u) | ((value >> 8u) & 0x00FF00FFu);
}

uint xe_swap_16_in_32(uint value) {
  return ((value & 0x0000FFFFu) << 16u) | (value >> 16u);
}

void main() {
  uint value = uint(gl_VertexIndex);
  uint endian = xe_system_cbuffer.xe_vertex_index_endian;
  if (endian == 1u || endian == 2u || endian == 3u) {
    value = xe_swap_8_in_16(value);
  }
  if (endian == 2u || endian == 3u) {
    value = xe_swap_16_in_32(value);
  }
  float tessellation_factor = float(value) + 1.0;
  tessellation_factor = max(
      tessellation_factor, xe_system_cbuffer.xe_tessellation_factor_range_min);
  tessellation_factor = min(
      tessellation_factor, xe_system_cbuffer.xe_tessellation_factor_range_max);
  xe_out_value = tessellation_factor;
}
)";
  return source;
}

std::string GetTessellationControlShaderGlsl(Shader::HostVertexShaderType host_vertex_shader_type,
                                             xenos::TessellationMode tessellation_mode,
                                             uint32_t input_control_points,
                                             uint32_t output_control_points) {
  std::string source;
  source += "#version 450\n";
  source += GetTessellationSystemConstantsBlockGlsl();
  source += "layout(location = 0) in float xe_input_value[];\n";
  source += "layout(location = 0) patch out vec4 xe_out_patch_control_point_indices;\n";
  // In GLSL, primitive mode / spacing / winding are TES layout qualifiers.
  // TCS only supports `layout(vertices = N) out`.
  source += fmt::format("layout(vertices = {}) out;\n", output_control_points);

  source += R"(
void main() {
  gl_out[gl_InvocationID].gl_Position = vec4(0.0);
  if (gl_InvocationID != 0u) {
    return;
  }

  vec4 patch_control_point_indices = vec4(0.0);
)";
  if (tessellation_mode == xenos::TessellationMode::kAdaptive) {
    source += R"(
  uint patch_index = uint(int(gl_PrimitiveID) + xe_system_cbuffer.xe_vertex_base_index);
  patch_index &= 0x00FFFFFFu;
  patch_index = max(patch_index, xe_system_cbuffer.xe_vertex_index_min);
  patch_index = min(patch_index, xe_system_cbuffer.xe_vertex_index_max);
  patch_control_point_indices.x = float(patch_index);
)";
    if (IsTriangleDomainHostVertexShaderType(host_vertex_shader_type)) {
      source += R"(
  gl_TessLevelOuter[0] = xe_input_value[1];
  gl_TessLevelOuter[1] = xe_input_value[2];
  gl_TessLevelOuter[2] = xe_input_value[0];
  gl_TessLevelInner[0] = min(min(xe_input_value[2], xe_input_value[1]),
                             xe_input_value[0]);
)";
    } else {
      source += R"(
  gl_TessLevelOuter[0] = xe_input_value[3];
  gl_TessLevelOuter[1] = xe_input_value[0];
  gl_TessLevelOuter[2] = xe_input_value[1];
  gl_TessLevelOuter[3] = xe_input_value[2];
  gl_TessLevelInner[0] = min(xe_input_value[2], xe_input_value[0]);
  gl_TessLevelInner[1] = min(xe_input_value[1], xe_input_value[3]);
)";
    }
  } else {
    if (IsPatchIndexedHostVertexShaderType(host_vertex_shader_type)) {
      source += "  patch_control_point_indices.x = xe_input_value[0];\n";
    } else {
      source += "  patch_control_point_indices.x = xe_input_value[0];\n";
      if (input_control_points >= 2) {
        source += "  patch_control_point_indices.y = xe_input_value[1];\n";
      }
      if (input_control_points >= 3) {
        source += "  patch_control_point_indices.z = xe_input_value[2];\n";
      }
      if (input_control_points >= 4) {
        source += "  patch_control_point_indices.w = xe_input_value[3];\n";
      }
    }
    source += R"(
  float tessellation_factor = xe_system_cbuffer.xe_tessellation_factor_range_max;
)";
    if (IsTriangleDomainHostVertexShaderType(host_vertex_shader_type)) {
      source += R"(
  gl_TessLevelOuter[0] = tessellation_factor;
  gl_TessLevelOuter[1] = tessellation_factor;
  gl_TessLevelOuter[2] = tessellation_factor;
  gl_TessLevelInner[0] = tessellation_factor;
)";
    } else {
      source += R"(
  gl_TessLevelOuter[0] = tessellation_factor;
  gl_TessLevelOuter[1] = tessellation_factor;
  gl_TessLevelOuter[2] = tessellation_factor;
  gl_TessLevelOuter[3] = tessellation_factor;
  gl_TessLevelInner[0] = tessellation_factor;
  gl_TessLevelInner[1] = tessellation_factor;
)";
    }
  }
  source += R"(
  xe_out_patch_control_point_indices = patch_control_point_indices;
}
)";
  return source;
}

}  // namespace

VulkanPipelineCache::VulkanPipelineCache(VulkanCommandProcessor& command_processor,
                                         const RegisterFile& register_file,
                                         VulkanRenderTargetCache& render_target_cache,
                                         VkShaderStageFlags guest_shader_vertex_stages)
    : command_processor_(command_processor),
      register_file_(register_file),
      render_target_cache_(render_target_cache),
      guest_shader_vertex_stages_(guest_shader_vertex_stages) {}

VulkanPipelineCache::~VulkanPipelineCache() {
  Shutdown();
}

bool VulkanPipelineCache::Initialize() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  {
    const ui::vulkan::VulkanDevice::Properties& p = vulkan_device->properties();
    uses_pipeline_libraries_ =
        REXCVAR_GET(vulkan_pipeline_libraries) && !edram_fragment_shader_interlock &&
        REXCVAR_GET(vulkan_dynamic_rendering) && p.dynamicRendering &&
        p.graphicsPipelineLibrary && p.graphicsPipelineLibraryFastLinking &&
        p.extendedDynamicState && p.extendedDynamicState2 &&
        p.extendedDynamicState3PolygonMode && p.extendedDynamicState3DepthClampEnable &&
        p.extendedDynamicState3ColorBlendEnable && p.extendedDynamicState3ColorBlendEquation &&
        p.extendedDynamicState3ColorWriteMask;
    // Pairing-independent translations need interpolator I at location I in every stage.
    SpirvShaderTranslator::SetFixedInterpolatorLocations(uses_pipeline_libraries_);
    // Eco mode (gpu_eco_shading_rate): the shading rate as dynamic state of every guest pipeline;
    // the diagnostic vulkan_fragment_shading_rate keeps its fixed rate instead.
    dynamic_fragment_shading_rate_ = vulkan_device->extensions().ext_KHR_fragment_shading_rate &&
                                     REXCVAR_GET(vulkan_dynamic_fragment_shading_rate) &&
                                     REXCVAR_GET(vulkan_fragment_shading_rate).empty();
    REXGPU_INFO("VulkanPipelineCache: fragment shading rate {}",
                dynamic_fragment_shading_rate_ ? "dynamic (Eco mode available)" : "fixed");
    REXGPU_INFO("VulkanPipelineCache: guest pipelines {}",
                uses_pipeline_libraries_
                    ? "linked from per-stage pipeline libraries (fast linking, dynamic state)"
                    : "created whole");
  }

  shader_translator_ = std::make_unique<SpirvShaderTranslator>(
      SpirvShaderTranslator::Features(vulkan_device),
      render_target_cache_.msaa_2x_attachments_supported(),
      render_target_cache_.msaa_2x_no_attachments_supported(), edram_fragment_shader_interlock,
      render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y());
  async_shader_translator_ = std::make_unique<SpirvShaderTranslator>(
      SpirvShaderTranslator::Features(vulkan_device),
      render_target_cache_.msaa_2x_attachments_supported(),
      render_target_cache_.msaa_2x_no_attachments_supported(), edram_fragment_shader_interlock,
      render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y());

  if (edram_fragment_shader_interlock) {
    std::vector<uint8_t> depth_only_fragment_shader_code =
        shader_translator_->CreateDepthOnlyFragmentShader();
    depth_only_fragment_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, reinterpret_cast<const uint32_t*>(depth_only_fragment_shader_code.data()),
        depth_only_fragment_shader_code.size());
    if (depth_only_fragment_shader_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanPipelineCache: Failed to create the depth/stencil-only "
          "fragment shader for the fragment shader interlock render backend "
          "implementation");
      return false;
    }
  } else {
    std::vector<uint8_t> depth_float24_truncate_fragment_shader_code =
        shader_translator_->CreateDepthOnlyFragmentShader(
            SpirvShaderTranslator::Modification::DepthStencilMode::kFloat24Truncating);
    depth_float24_truncate_fragment_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device,
        reinterpret_cast<const uint32_t*>(depth_float24_truncate_fragment_shader_code.data()),
        depth_float24_truncate_fragment_shader_code.size());
    if (depth_float24_truncate_fragment_shader_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanPipelineCache: Failed to create the float24 truncating "
          "depth-only fragment shader");
      return false;
    }
    std::vector<uint8_t> depth_float24_round_fragment_shader_code =
        shader_translator_->CreateDepthOnlyFragmentShader(
            SpirvShaderTranslator::Modification::DepthStencilMode::kFloat24Rounding);
    depth_float24_round_fragment_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device,
        reinterpret_cast<const uint32_t*>(depth_float24_round_fragment_shader_code.data()),
        depth_float24_round_fragment_shader_code.size());
    if (depth_float24_round_fragment_shader_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanPipelineCache: Failed to create the float24 rounding "
          "depth-only fragment shader");
      return false;
    }
  }

  std::vector<uint32_t> placeholder_pixel_shader_spirv;
  std::string placeholder_pixel_shader_compile_error;
  if (command_processor_.CompileGlslToSpirv(
          VK_SHADER_STAGE_FRAGMENT_BIT, kPlaceholderPixelShaderSource,
          placeholder_pixel_shader_spirv, placeholder_pixel_shader_compile_error)) {
    placeholder_pixel_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, placeholder_pixel_shader_spirv.data(),
        placeholder_pixel_shader_spirv.size() * sizeof(uint32_t));
  } else {
    REXGPU_WARN("VulkanPipelineCache: Failed to compile placeholder pixel shader: {}",
                placeholder_pixel_shader_compile_error);
  }
  if (placeholder_pixel_shader_ == VK_NULL_HANDLE) {
    REXGPU_WARN(
        "VulkanPipelineCache: Failed to create placeholder pixel shader - "
        "async placeholder hot-swap will be unavailable");
  }

  uint32_t logical_processor_count = rex::thread::logical_processor_count();
  if (!logical_processor_count) {
    logical_processor_count = 6;
  }
  creation_threads_busy_ = 0;
  startup_loading_ = false;
  creation_completion_event_ = rex::thread::Event::CreateManualResetEvent(true);
  assert_not_null(creation_completion_event_);
  creation_completion_set_event_ = false;
  creation_threads_shutdown_from_ = SIZE_MAX;
  if (REXCVAR_GET(vulkan_pipeline_creation_threads) != 0) {
    size_t creation_thread_count;
    if (REXCVAR_GET(vulkan_pipeline_creation_threads) < 0) {
      creation_thread_count = std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      creation_thread_count = std::min(uint32_t(REXCVAR_GET(vulkan_pipeline_creation_threads)),
                                       logical_processor_count);
    }
    for (size_t i = 0; i < creation_thread_count; ++i) {
      std::unique_ptr<rex::thread::Thread> creation_thread =
          rex::thread::Thread::Create({}, [this, i]() { CreationThread(i); });
      assert_not_null(creation_thread);
      creation_thread->set_name("Vulkan Pipelines");
      creation_threads_.push_back(std::move(creation_thread));
    }
  }

  return true;
}

void VulkanPipelineCache::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                  uint32_t title_id, bool blocking) {
  ShutdownShaderStorage();
  {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    startup_loading_ = false;
  }
  const auto storage_start_time = std::chrono::steady_clock::now();
  auto storage_ms_since_start = [&storage_start_time]() {
    return double(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - storage_start_time)
                      .count()) /
           1000.0;
  };

  auto shader_storage_root = cache_root / "shaders";
  auto shader_storage_shareable_root = shader_storage_root / "shareable";
  if (!std::filesystem::exists(shader_storage_shareable_root)) {
    if (!std::filesystem::create_directories(shader_storage_shareable_root)) {
      REXGPU_ERROR(
          "Failed to create the shareable shader storage directory, persistent "
          "shader storage will be disabled: {}",
          rex::path_to_utf8(shader_storage_shareable_root));
      return;
    }
  }

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  // Initialize the pipeline storage stream - read pipeline descriptions and
  // collect used shader modifications to translate.
  std::vector<PipelineStoredDescription> pipeline_stored_descriptions;
  // <Shader hash, modification bits>.
  std::set<std::pair<uint64_t, uint64_t>> shader_translations_needed;
  auto pipeline_storage_file_path =
      shader_storage_shareable_root /
      fmt::format("{:08X}.{}.vk.xpso", title_id, edram_fragment_shader_interlock ? "fsi" : "fbo");
  pipeline_storage_file_ = OpenStorageFileForAppend(pipeline_storage_file_path);
  if (!pipeline_storage_file_) {
    REXGPU_ERROR(
        "Failed to open the Vulkan pipeline description storage file for "
        "writing, persistent shader storage will be disabled: {}",
        rex::path_to_utf8(pipeline_storage_file_path));
    return;
  }
  // "a+" leaves the read position at the end of the file on Bionic (and the other BSD-derived C
  // libraries; glibc and MSVC start at the beginning), so the header read below failed and the
  // storage was reset at every start on Android. Writes still always go to the end.
  rex::filesystem::Seek(pipeline_storage_file_, 0, SEEK_SET);
  pipeline_storage_file_flush_needed_ = false;
  // 'XEPS'.
  const uint32_t pipeline_storage_magic = 0x53504558;
  const uint32_t pipeline_storage_magic_api = edram_fragment_shader_interlock ? 1u : 0u;
  const uint32_t pipeline_storage_version_swapped = rex::byte_swap(
      std::max(PipelineDescription::kVersion, SpirvShaderTranslator::Modification::kVersion));
  struct {
    uint32_t magic;
    uint32_t magic_api;
    uint32_t version_swapped;
  } pipeline_storage_file_header;
  // Descriptions in the file before the first corrupted one (including those skipped as
  // unsupported on this device, which stay in the file).
  size_t pipeline_storage_valid_count = 0;
  if (fread(&pipeline_storage_file_header, sizeof(pipeline_storage_file_header), 1,
            pipeline_storage_file_) &&
      pipeline_storage_file_header.magic == pipeline_storage_magic &&
      pipeline_storage_file_header.magic_api == pipeline_storage_magic_api &&
      pipeline_storage_file_header.version_swapped == pipeline_storage_version_swapped) {
    rex::filesystem::Seek(pipeline_storage_file_, 0, SEEK_END);
    int64_t pipeline_storage_told_end = rex::filesystem::Tell(pipeline_storage_file_);
    size_t pipeline_storage_told_count =
        size_t(pipeline_storage_told_end >= int64_t(sizeof(pipeline_storage_file_header))
                   ? (uint64_t(pipeline_storage_told_end) - sizeof(pipeline_storage_file_header)) /
                         sizeof(PipelineStoredDescription)
                   : 0);
    if (pipeline_storage_told_count &&
        rex::filesystem::Seek(pipeline_storage_file_, int64_t(sizeof(pipeline_storage_file_header)),
                              SEEK_SET)) {
      pipeline_stored_descriptions.resize(pipeline_storage_told_count);
      pipeline_stored_descriptions.resize(
          fread(pipeline_stored_descriptions.data(), sizeof(PipelineStoredDescription),
                pipeline_storage_told_count, pipeline_storage_file_));
      size_t pipeline_storage_read_count = pipeline_stored_descriptions.size();
      size_t pipeline_storage_kept_count = 0;
      size_t pipeline_storage_skipped_unsupported_count = 0;
      size_t pipeline_storage_skipped_corrupted_count = 0;
      for (size_t i = 0; i < pipeline_storage_read_count; ++i) {
        const PipelineStoredDescription pipeline_stored_description =
            pipeline_stored_descriptions[i];
        // Validate file integrity, stop and truncate the stream if data is
        // corrupted.
        if (XXH3_64bits(&pipeline_stored_description.description,
                        sizeof(pipeline_stored_description.description)) !=
            pipeline_stored_description.description_hash) {
          pipeline_storage_skipped_corrupted_count = pipeline_storage_read_count - i;
          pipeline_storage_read_count = i;
          break;
        }
        if (!ArePipelineRequirementsMet(pipeline_stored_description.description)) {
          ++pipeline_storage_skipped_unsupported_count;
          continue;
        }
        pipeline_stored_descriptions[pipeline_storage_kept_count++] = pipeline_stored_description;
        // Mark the shader modifications as needed for translation.
        shader_translations_needed.emplace(
            pipeline_stored_description.description.vertex_shader_hash,
            pipeline_stored_description.description.vertex_shader_modification);
        if (pipeline_stored_description.description.pixel_shader_hash) {
          shader_translations_needed.emplace(
              pipeline_stored_description.description.pixel_shader_hash,
              pipeline_stored_description.description.pixel_shader_modification);
        }
      }
      pipeline_storage_valid_count = pipeline_storage_read_count;
      pipeline_stored_descriptions.resize(pipeline_storage_kept_count);
      if (pipeline_storage_skipped_unsupported_count || pipeline_storage_skipped_corrupted_count) {
        REXGPU_INFO(
            "VulkanPipelineCache: Pipeline storage filtering kept {} descriptions (skipped {} "
            "unsupported, {} corrupted)",
            pipeline_storage_kept_count, pipeline_storage_skipped_unsupported_count,
            pipeline_storage_skipped_corrupted_count);
      }
    }
  }

  // Initialize the Xenos shader storage stream.
  auto shader_storage_file_path =
      shader_storage_shareable_root / fmt::format("{:08X}.xsh", title_id);
  shader_storage_file_ = OpenStorageFileForAppend(shader_storage_file_path);
  if (!shader_storage_file_) {
    REXGPU_ERROR(
        "Failed to open the guest shader storage file for writing, persistent "
        "shader storage will be disabled: {}",
        rex::path_to_utf8(shader_storage_file_path));
    fclose(pipeline_storage_file_);
    pipeline_storage_file_ = nullptr;
    return;
  }
  // See the pipeline storage: "a+" starts reading at the end on Bionic.
  rex::filesystem::Seek(shader_storage_file_, 0, SEEK_SET);
  ++shader_storage_index_;
  shader_storage_file_flush_needed_ = false;
  struct {
    uint32_t magic;
    uint32_t version_swapped;
  } shader_storage_file_header;
  // 'XESH'.
  const uint32_t shader_storage_magic = 0x48534558;
  size_t shader_storage_loaded_count = 0;
  if (fread(&shader_storage_file_header, sizeof(shader_storage_file_header), 1,
            shader_storage_file_) &&
      shader_storage_file_header.magic == shader_storage_magic &&
      rex::byte_swap(shader_storage_file_header.version_swapped) == ShaderStoredHeader::kVersion) {
    uint64_t shader_storage_valid_bytes = sizeof(shader_storage_file_header);
    // Load shaders written by previous runs until the end of the file or until
    // a corrupted one is detected.
    ShaderStoredHeader shader_header;
    std::vector<uint32_t> ucode_dwords;
    ucode_dwords.reserve(0xFFFF);
    while (true) {
      if (!fread(&shader_header, sizeof(shader_header), 1, shader_storage_file_)) {
        break;
      }
      size_t ucode_byte_count = shader_header.ucode_dword_count * sizeof(uint32_t);
      ucode_dwords.resize(shader_header.ucode_dword_count);
      if (shader_header.ucode_dword_count &&
          !fread(ucode_dwords.data(), ucode_byte_count, 1, shader_storage_file_)) {
        break;
      }
      uint64_t ucode_data_hash = XXH3_64bits(ucode_dwords.data(), ucode_byte_count);
      if (shader_header.ucode_data_hash != ucode_data_hash) {
        // Validation failed.
        break;
      }
      shader_storage_valid_bytes += sizeof(shader_header) + ucode_byte_count;
      VulkanShader* shader = LoadShader(shader_header.type, ucode_dwords.data(),
                                        shader_header.ucode_dword_count, ucode_data_hash);
      if (uses_pipeline_libraries_ && shader_header.type == xenos::ShaderType::kPixel) {
        precompile_pixel_shaders_.push_back({ucode_data_hash, ucode_dwords});
      } else if (uses_pipeline_libraries_ && shader_header.type == xenos::ShaderType::kVertex &&
                 REXCVAR_GET(vulkan_precompile_vertex_libraries)) {
        precompile_vertex_shaders_.push_back({ucode_data_hash, ucode_dwords});
      }
      if (shader->ucode_storage_index() == shader_storage_index_) {
        // Appeared twice in this file for some reason - skip.
        continue;
      }
      // Loaded from the current storage - don't write again.
      shader->set_ucode_storage_index(shader_storage_index_);
      ++shader_storage_loaded_count;
    }
    rex::filesystem::TruncateStdioFile(shader_storage_file_, shader_storage_valid_bytes);
  } else {
    rex::filesystem::TruncateStdioFile(shader_storage_file_, 0);
    shader_storage_file_header.magic = shader_storage_magic;
    shader_storage_file_header.version_swapped = rex::byte_swap(ShaderStoredHeader::kVersion);
    fwrite(&shader_storage_file_header, sizeof(shader_storage_file_header), 1,
           shader_storage_file_);
  }

  REXGPU_INFO(
      "VulkanPipelineCache: Storage has {} shaders and {} pipeline descriptions from previous "
      "runs ({})",
      shader_storage_loaded_count, pipeline_stored_descriptions.size(),
      rex::path_to_utf8(shader_storage_shareable_root));

  // Before creating the stored pipelines, so they come from the driver's cache when possible.
  LoadDriverPipelineCache(GetDriverPipelineCachePath(cache_root, title_id));
  if (uses_pipeline_libraries_) {
    LoadLibraryKeys(shader_storage_shareable_root / fmt::format("{:08X}.vk.libkeys", title_id));
    // Pixel shader metadata from tools/lr_shader_scan.py: 'XEPM', count, (u64 hash, u32 header
    // word 3) records - bit 1: parameter generation, bits 8-15: its interpolator.
    if (FILE* file = rex::filesystem::OpenFile(
            shader_storage_shareable_root / fmt::format("{:08X}.psmeta", title_id), "rb")) {
      uint32_t header[2];
      if (fread(header, sizeof(header), 1, file) && header[0] == 0x4D504558) {
        for (uint32_t i = 0; i < header[1]; ++i) {
          uint64_t hash;
          uint32_t field;
          if (!fread(&hash, sizeof(hash), 1, file) || !fread(&field, sizeof(field), 1, file)) {
            break;
          }
          pixel_shader_meta_.emplace(hash, field);
        }
      }
      fclose(file);
    }
    precompile_marker_path_ =
        shader_storage_shareable_root / fmt::format("{:08X}.vk.libprecompiled", title_id);
  }

  const double storage_ms_read = storage_ms_since_start();
  uint32_t storage_logical_processor_count = rex::thread::logical_processor_count();
  if (!storage_logical_processor_count) {
    storage_logical_processor_count = 6;
  }

  // Translate shader modifications needed for stored pipelines. Each shader with all its
  // modifications goes to one thread (the shader's analysis, translation list and binding setup
  // are per shader and not thread-safe), every thread has its own translator.
  std::vector<std::pair<VulkanShader*, std::vector<uint64_t>>> storage_translation_work;
  for (const std::pair<uint64_t, uint64_t>& translation_needed : shader_translations_needed) {
    auto shader_it = shaders_.find(translation_needed.first);
    if (shader_it == shaders_.end()) {
      continue;
    }
    if (storage_translation_work.empty() ||
        storage_translation_work.back().first != shader_it->second) {
      storage_translation_work.emplace_back(shader_it->second, std::vector<uint64_t>());
    }
    storage_translation_work.back().second.push_back(translation_needed.second);
  }
  size_t storage_translation_thread_count;
  if (REXCVAR_GET(vulkan_storage_translation_threads) > 0) {
    storage_translation_thread_count =
        std::min(uint32_t(REXCVAR_GET(vulkan_storage_translation_threads)),
                 storage_logical_processor_count);
  } else if (REXCVAR_GET(vulkan_pipeline_creation_threads) > 0) {
    storage_translation_thread_count =
        std::min(uint32_t(REXCVAR_GET(vulkan_pipeline_creation_threads)),
                 storage_logical_processor_count);
  } else if (REXCVAR_GET(vulkan_pipeline_creation_threads) < 0) {
    storage_translation_thread_count =
        std::max(storage_logical_processor_count * 3 / 4, uint32_t(1));
  } else {
    storage_translation_thread_count = 1;
  }
  // Below a few shaders per thread the thread and translator setup costs more than it saves.
  storage_translation_thread_count = std::max(
      std::min(storage_translation_thread_count, storage_translation_work.size() / 8), size_t(1));
  std::atomic<size_t> storage_translation_next(0);
  std::atomic<size_t> storage_translation_count(0);
  auto storage_translation_worker = [this, &storage_translation_work, &storage_translation_next,
                                     &storage_translation_count](
                                        SpirvShaderTranslator& translator,
                                        string::StringBuffer& ucode_disasm_buffer) {
    while (true) {
      size_t work_index = storage_translation_next.fetch_add(1);
      if (work_index >= storage_translation_work.size()) {
        break;
      }
      VulkanShader* shader = storage_translation_work[work_index].first;
      if (!shader->is_ucode_analyzed()) {
        shader->AnalyzeUcode(ucode_disasm_buffer);
      }
      for (uint64_t modification : storage_translation_work[work_index].second) {
        bool translation_is_new = false;
        auto* translation = static_cast<VulkanShader::VulkanTranslation*>(
            shader->GetOrCreateTranslation(modification, &translation_is_new));
        if (!translation->is_translated() &&
            !TranslateAnalyzedShader(translator, *translation)) {
          if (translation_is_new) {
            shader->DestroyTranslation(modification);
          }
          continue;
        }
        storage_translation_count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };
  if (storage_translation_thread_count <= 1) {
    storage_translation_worker(*shader_translator_, ucode_disasm_buffer_);
  } else {
    const ui::vulkan::VulkanDevice* const translation_vulkan_device =
        command_processor_.GetVulkanDevice();
    std::vector<std::unique_ptr<rex::thread::Thread>> translation_threads;
    translation_threads.reserve(storage_translation_thread_count);
    for (size_t i = 0; i < storage_translation_thread_count; ++i) {
      std::unique_ptr<rex::thread::Thread> translation_thread = rex::thread::Thread::Create(
          {}, [this, translation_vulkan_device, edram_fragment_shader_interlock,
               &storage_translation_worker]() {
            SpirvShaderTranslator translator(
                SpirvShaderTranslator::Features(translation_vulkan_device),
                render_target_cache_.msaa_2x_attachments_supported(),
                render_target_cache_.msaa_2x_no_attachments_supported(),
                edram_fragment_shader_interlock, render_target_cache_.draw_resolution_scale_x(),
                render_target_cache_.draw_resolution_scale_y());
            string::StringBuffer ucode_disasm_buffer;
            storage_translation_worker(translator, ucode_disasm_buffer);
          });
      assert_not_null(translation_thread);
      translation_thread->set_name("Vulkan Shader Storage");
      translation_threads.push_back(std::move(translation_thread));
    }
    for (const auto& translation_thread : translation_threads) {
      rex::thread::Wait(translation_thread.get(), false);
    }
  }
  const double storage_ms_translated = storage_ms_since_start();

  if (REXCVAR_GET(vulkan_storage_translate_all)) {
    std::vector<VulkanShader*> all_shaders;
    all_shaders.reserve(shaders_.size());
    for (auto& shader_pair : shaders_) {
      all_shaders.push_back(shader_pair.second);
    }
    std::atomic<size_t> all_next(0), all_ok(0), all_failed(0);
    std::mutex all_failed_lock;
    std::vector<std::pair<uint64_t, xenos::ShaderType>> all_failed_list;
    auto all_worker = [&](SpirvShaderTranslator& translator,
                          string::StringBuffer& ucode_disasm_buffer) {
      while (true) {
        size_t index = all_next.fetch_add(1);
        if (index >= all_shaders.size()) {
          break;
        }
        VulkanShader* shader = all_shaders[index];
        if (!shader->is_ucode_analyzed()) {
          shader->AnalyzeUcode(ucode_disasm_buffer);
        }
        uint64_t modification_value;
        if (shader->type() == xenos::ShaderType::kVertex) {
          SpirvShaderTranslator::Modification modification(
              translator.GetDefaultVertexShaderModification(
                  shader->GetDynamicAddressableRegisterCount(127)));
          modification.vertex.interpolator_mask = shader->writes_interpolators();
          modification_value = modification.value;
        } else {
          SpirvShaderTranslator::Modification modification(
              translator.GetDefaultPixelShaderModification(
                  shader->GetDynamicAddressableRegisterCount(127)));
          modification.pixel.interpolator_mask = 0xFFFF;
          modification_value = modification.value;
        }
        bool translation_is_new = false;
        auto* translation = static_cast<VulkanShader::VulkanTranslation*>(
            shader->GetOrCreateTranslation(modification_value, &translation_is_new));
        if (translation->is_translated() || TranslateAnalyzedShader(translator, *translation)) {
          all_ok.fetch_add(1, std::memory_order_relaxed);
        } else {
          all_failed.fetch_add(1, std::memory_order_relaxed);
          std::lock_guard<std::mutex> lock(all_failed_lock);
          all_failed_list.emplace_back(shader->ucode_data_hash(), shader->type());
        }
      }
    };
    const ui::vulkan::VulkanDevice* const all_vulkan_device = command_processor_.GetVulkanDevice();
    std::vector<std::unique_ptr<rex::thread::Thread>> all_threads;
    size_t all_thread_count = std::max(storage_translation_thread_count, size_t(1));
    for (size_t i = 0; i < all_thread_count; ++i) {
      all_threads.push_back(rex::thread::Thread::Create(
          {}, [this, all_vulkan_device, edram_fragment_shader_interlock, &all_worker]() {
            SpirvShaderTranslator translator(
                SpirvShaderTranslator::Features(all_vulkan_device),
                render_target_cache_.msaa_2x_attachments_supported(),
                render_target_cache_.msaa_2x_no_attachments_supported(),
                edram_fragment_shader_interlock, render_target_cache_.draw_resolution_scale_x(),
                render_target_cache_.draw_resolution_scale_y());
            string::StringBuffer ucode_disasm_buffer;
            all_worker(translator, ucode_disasm_buffer);
          }));
    }
    for (const auto& thread : all_threads) {
      rex::thread::Wait(thread.get(), false);
    }
    REXGPU_INFO(
        "Translated every stored shader (vulkan_storage_translate_all): {} shaders, {} ok, {} "
        "failed, {:.0f} ms on {} threads",
        all_shaders.size(), all_ok.load(), all_failed.load(),
        storage_ms_since_start() - storage_ms_translated, all_thread_count);
    for (size_t i = 0; i < all_failed_list.size(); ++i) {
      REXGPU_INFO("Translation failed: {} shader {:016X}",
                  all_failed_list[i].second == xenos::ShaderType::kVertex ? "vertex" : "pixel",
                  all_failed_list[i].first);
    }
  }

  // Create the pipelines.
  std::vector<PipelineCreationArguments> pipeline_creations;
  pipeline_creations.reserve(pipeline_stored_descriptions.size());
  for (const PipelineStoredDescription& pipeline_stored_description :
       pipeline_stored_descriptions) {
    const PipelineDescription& pipeline_description = pipeline_stored_description.description;

    auto find_it = pipelines_.find(pipeline_description);
    if (find_it != pipelines_.end()) {
      continue;
    }
    auto& pipeline = *pipelines_.emplace(pipeline_description, Pipeline()).first;
    PipelineCreationArguments creation_arguments;
    if (!TryGetPipelineCreationArgumentsForDescription(pipeline_description, &pipeline,
                                                       creation_arguments)) {
      pipelines_.erase(pipeline_description);
      continue;
    }
    uint32_t bound_rts = 0;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (pipeline_description.render_targets[i].color_write_mask) {
        bound_rts |= uint32_t(1) << i;
      }
    }
    uint32_t shader_writes_color_targets =
        creation_arguments.pixel_shader
            ? creation_arguments.pixel_shader->shader().writes_color_targets()
            : 0;
    bool shader_writes_depth = creation_arguments.pixel_shader
                                   ? creation_arguments.pixel_shader->shader().writes_depth()
                                   : pipeline_description.depth_write_enable != 0;
    creation_arguments.priority = pipeline_util::CalculatePipelinePriority(
        bound_rts, shader_writes_color_targets, shader_writes_depth);
    pipeline_creations.push_back(creation_arguments);
  }

  if (!pipeline_creations.empty()) {
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      startup_loading_ = true;
    }
    uint32_t logical_processor_count = rex::thread::logical_processor_count();
    if (!logical_processor_count) {
      // Pick some reasonable amount if couldn't determine the number of cores.
      logical_processor_count = 6;
    }
    size_t creation_thread_count;
    if (REXCVAR_GET(vulkan_pipeline_creation_threads) == 0) {
      creation_thread_count = 1;
    } else if (REXCVAR_GET(vulkan_pipeline_creation_threads) < 0) {
      creation_thread_count = std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      creation_thread_count = std::min(uint32_t(REXCVAR_GET(vulkan_pipeline_creation_threads)),
                                       logical_processor_count);
    }
    if (!blocking) {
      // Vulkan currently doesn't support deferred pipeline handles at draw time,
      // so complete preload creation regardless of blocking mode.
      creation_thread_count = std::max(creation_thread_count, size_t(1));
    }
    creation_thread_count = std::min(creation_thread_count, pipeline_creations.size());

    std::atomic<size_t> next_creation_index(0);
    std::atomic<size_t> created_pipeline_count(0);
    auto creation_worker = [this, &pipeline_creations, &next_creation_index,
                            &created_pipeline_count]() {
      while (true) {
        size_t creation_index = next_creation_index.fetch_add(1);
        if (creation_index >= pipeline_creations.size()) {
          break;
        }
        if (EnsurePipelineCreated(pipeline_creations[creation_index])) {
          created_pipeline_count.fetch_add(1);
        }
      }
    };

    if (creation_thread_count <= 1) {
      creation_worker();
    } else {
      std::vector<std::unique_ptr<rex::thread::Thread>> creation_threads;
      creation_threads.reserve(creation_thread_count);
      for (size_t i = 0; i < creation_thread_count; ++i) {
        std::unique_ptr<rex::thread::Thread> creation_thread =
            rex::thread::Thread::Create({}, creation_worker);
        assert_not_null(creation_thread);
        creation_thread->set_name("Vulkan Pipelines");
        creation_threads.push_back(std::move(creation_thread));
      }
      for (const auto& creation_thread : creation_threads) {
        rex::thread::Wait(creation_thread.get(), false);
      }
    }

    REXGPU_INFO("Created {} graphics pipelines from Vulkan storage ({} requested)",
                created_pipeline_count.load(), pipeline_creations.size());
    const double storage_ms_created = storage_ms_since_start();
    REXGPU_INFO(
        "Shader storage loaded in {:.0f} ms: reading and the driver cache {:.0f}, {} shader "
        "translations of {} shaders {:.0f} on {} threads, {} pipelines {:.0f} on {} threads",
        storage_ms_created, storage_ms_read, storage_translation_count.load(),
        storage_translation_work.size(), storage_ms_translated - storage_ms_read,
        storage_translation_thread_count, created_pipeline_count.load(),
        storage_ms_created - storage_ms_translated, creation_thread_count);
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      startup_loading_ = false;
    }
  }

  // If any pipeline descriptions were corrupted (or the whole file has excess
  // bytes in the end), truncate to the last valid pipeline description. Not to the count of the
  // descriptions kept for this device, which would cut valid ones if any were skipped.
  rex::filesystem::TruncateStdioFile(
      pipeline_storage_file_,
      uint64_t(sizeof(pipeline_storage_file_header) +
               sizeof(PipelineStoredDescription) * pipeline_storage_valid_count));
  if (!pipeline_storage_valid_count) {
    rex::filesystem::TruncateStdioFile(pipeline_storage_file_, 0);
    pipeline_storage_file_header.magic = pipeline_storage_magic;
    pipeline_storage_file_header.magic_api = pipeline_storage_magic_api;
    pipeline_storage_file_header.version_swapped = pipeline_storage_version_swapped;
    fwrite(&pipeline_storage_file_header, sizeof(pipeline_storage_file_header), 1,
           pipeline_storage_file_);
  }

  shader_storage_cache_root_ = cache_root;
  shader_storage_title_id_ = title_id;

  // Start the storage writing thread.
  storage_write_flush_shaders_ = false;
  storage_write_flush_pipelines_ = false;
  storage_write_thread_shutdown_ = false;
  storage_write_thread_ = rex::thread::Thread::Create({}, [this]() { StorageWriteThread(); });
  assert_not_null(storage_write_thread_);
  storage_write_thread_->set_name("Vulkan Storage writer");

  if (uses_pipeline_libraries_ && !precompile_pixel_shaders_.empty() &&
      !pixel_shader_meta_.empty()) {
    // Pixel shaders used in play first, in first-use order (the stored pipelines): on the Thor
    // ~540 of the 7,375 scanned shaders, ~25 s of the ~350 s precompile.
    std::unordered_map<uint64_t, uint32_t> used_rank;
    for (const PipelineStoredDescription& stored : pipeline_stored_descriptions) {
      uint64_t hash = stored.description.pixel_shader_hash;
      if (hash && pixel_shader_meta_.count(hash)) {
        used_rank.emplace(hash, uint32_t(used_rank.size()));
      }
    }
    std::stable_sort(precompile_pixel_shaders_.begin(), precompile_pixel_shaders_.end(),
                     [&used_rank](const PrecompileShader& a, const PrecompileShader& b) {
                       auto ia = used_rank.find(a.hash), ib = used_rank.find(b.hash);
                       uint32_t ra = ia != used_rank.end() ? ia->second : UINT32_MAX;
                       uint32_t rb = ib != used_rank.end() ? ib->second : UINT32_MAX;
                       return ra < rb;
                     });
    precompile_priority_count_ = 0;
    for (const PrecompileShader& entry : precompile_pixel_shaders_) {
      if (!used_rank.count(entry.hash)) {
        break;
      }
      ++precompile_priority_count_;
    }
    // Vertex libraries: the variants of the stored pipelines (plain vertex shaders, no geometry
    // shader) first, in first-use order, then a canonical one (computed on the precompile thread,
    // modification 0 here) for every other stored vertex shader.
    precompile_vertex_variants_.clear();
    precompile_vertex_used_count_ = 0;
    if (!precompile_vertex_shaders_.empty()) {
      std::unordered_map<uint64_t, size_t> vertex_index;
      for (size_t i = 0; i < precompile_vertex_shaders_.size(); ++i) {
        vertex_index.emplace(precompile_vertex_shaders_[i].hash, i);
      }
      std::unordered_set<uint64_t> variant_seen;
      std::vector<bool> vertex_has_variant(precompile_vertex_shaders_.size(), false);
      for (const PipelineStoredDescription& stored : pipeline_stored_descriptions) {
        const PipelineDescription& d = stored.description;
        SpirvShaderTranslator::Modification modification(d.vertex_shader_modification);
        if (d.geometry_shader != PipelineGeometryShader::kNone ||
            d.primitive_topology == PipelinePrimitiveTopology::kPatchList ||
            modification.vertex.host_vertex_shader_type != Shader::HostVertexShaderType::kVertex) {
          continue;
        }
        auto it = vertex_index.find(d.vertex_shader_hash);
        if (it == vertex_index.end() ||
            !variant_seen.insert(d.vertex_shader_hash ^ (d.vertex_shader_modification * 31)).second) {
          continue;
        }
        precompile_vertex_variants_.push_back({it->second, d.vertex_shader_modification, true});
        vertex_has_variant[it->second] = true;
      }
      precompile_vertex_used_count_ = precompile_vertex_variants_.size();
      for (size_t i = 0; i < precompile_vertex_shaders_.size(); ++i) {
        if (!vertex_has_variant[i]) {
          precompile_vertex_variants_.push_back({i, 0, false});
        }
      }
    }
    precompile_shutdown_ = false;
    precompile_thread_ = rex::thread::Thread::Create({}, [this]() { PrecompileThread(); });
    if (precompile_thread_) {
      precompile_thread_->set_name("Vulkan Library Precompile");
    }
  } else {
    precompile_pixel_shaders_.clear();
  }
}

void VulkanPipelineCache::ShutdownShaderStorage() {
  if (precompile_thread_) {
    precompile_shutdown_ = true;
    rex::thread::Wait(precompile_thread_.get(), false);
    precompile_thread_.reset();
  }
  if (library_keys_file_) {
    fclose(library_keys_file_);
    library_keys_file_ = nullptr;
  }
  if (storage_write_thread_) {
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_thread_shutdown_ = true;
    }
    storage_write_request_cond_.notify_all();
    rex::thread::Wait(storage_write_thread_.get(), false);
    storage_write_thread_.reset();
  }
  storage_write_shader_queue_.clear();
  storage_write_pipeline_queue_.clear();

  if (pipeline_storage_file_) {
    fclose(pipeline_storage_file_);
    pipeline_storage_file_ = nullptr;
    pipeline_storage_file_flush_needed_ = false;
  }

  if (shader_storage_file_) {
    fclose(shader_storage_file_);
    shader_storage_file_ = nullptr;
    shader_storage_file_flush_needed_ = false;
  }

  SaveDriverPipelineCache();
  driver_pipeline_cache_path_.clear();

  shader_storage_cache_root_.clear();
  shader_storage_title_id_ = 0;
}

std::filesystem::path VulkanPipelineCache::GetDriverPipelineCachePath(
    const std::filesystem::path& cache_root, uint32_t title_id) {
  return cache_root / "shaders" / "shareable" / fmt::format("{:08X}.vk.pipelinecache", title_id);
}

size_t VulkanPipelineCache::MergeDriverPipelineCacheFile(
    const ui::vulkan::VulkanDevice* vulkan_device, const std::filesystem::path& path) {
  const VkPipelineCache device_pipeline_cache = vulkan_device->pipeline_cache();
  if (device_pipeline_cache == VK_NULL_HANDLE) {
    return 0;
  }
  std::error_code error_code;
  std::filesystem::path temp_path = path;
  temp_path += ".tmp";
  // Left by a save interrupted by a kill.
  std::filesystem::remove(temp_path, error_code);

  std::vector<uint8_t> data;
  if (FILE* file = rex::filesystem::OpenFile(path, "rb")) {
    rex::filesystem::Seek(file, 0, SEEK_END);
    int64_t file_size = rex::filesystem::Tell(file);
    rex::filesystem::Seek(file, 0, SEEK_SET);
    if (file_size > 0 && file_size < (int64_t(512) << 20)) {
      data.resize(size_t(file_size));
      if (fread(data.data(), 1, data.size(), file) != data.size()) {
        data.clear();
      }
    }
    fclose(file);
  }
  if (data.empty()) {
    REXGPU_INFO("VulkanPipelineCache: No driver pipeline cache in {}, starting empty",
                rex::path_to_utf8(path));
    return 0;
  }

  // The driver should ignore data from another GPU or driver build, but validate it here so a
  // driver update does not feed it foreign data.
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();
  const char* discard_reason = nullptr;
  VkPipelineCacheHeaderVersionOne header;
  if (data.size() < sizeof(header)) {
    discard_reason = "truncated header";
  } else {
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.headerSize < sizeof(header) || header.headerSize > data.size() ||
        header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) {
      discard_reason = "unknown header";
    } else if (header.vendorID != device_properties.vendorID ||
               header.deviceID != device_properties.deviceID ||
               std::memcmp(header.pipelineCacheUUID, device_properties.pipelineCacheUUID,
                           VK_UUID_SIZE)) {
      discard_reason = "made by another GPU or driver build";
    }
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (!discard_reason) {
    VkPipelineCacheCreateInfo pipeline_cache_create_info = {};
    pipeline_cache_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    pipeline_cache_create_info.initialDataSize = data.size();
    pipeline_cache_create_info.pInitialData = data.data();
    VkPipelineCache loaded_pipeline_cache = VK_NULL_HANDLE;
    if (dfn.vkCreatePipelineCache(device, &pipeline_cache_create_info, nullptr,
                                  &loaded_pipeline_cache) != VK_SUCCESS) {
      discard_reason = "rejected by the driver";
    } else {
      if (dfn.vkMergePipelineCaches(device, device_pipeline_cache, 1, &loaded_pipeline_cache) !=
          VK_SUCCESS) {
        discard_reason = "merge failed";
      }
      dfn.vkDestroyPipelineCache(device, loaded_pipeline_cache, nullptr);
    }
  }
  if (discard_reason) {
    REXGPU_INFO("VulkanPipelineCache: Discarded the driver pipeline cache {} ({} KB): {}",
                rex::path_to_utf8(path), data.size() / 1024, discard_reason);
    return 0;
  }
  REXGPU_INFO(
      "VulkanPipelineCache: Loaded the driver pipeline cache: {} KB from {} (vendor 0x{:04X}, "
      "device 0x{:08X})",
      data.size() / 1024, rex::path_to_utf8(path), header.vendorID, header.deviceID);
  return data.size();
}

void VulkanPipelineCache::LoadDriverPipelineCache(const std::filesystem::path& path) {
  driver_pipeline_cache_path_.clear();
  driver_pipeline_cache_saved_size_ = 0;
  driver_pipeline_cache_first_unsaved_use_ms_.store(0, std::memory_order_relaxed);
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  if (vulkan_device->pipeline_cache() == VK_NULL_HANDLE) {
    return;
  }
  driver_pipeline_cache_path_ = path;
  // Merged already at the command processor setup, before the built-in pipelines - don't merge
  // the same data again.
  size_t merged_size = command_processor_.setup_driver_pipeline_cache_path() == path
                           ? command_processor_.setup_driver_pipeline_cache_size()
                           : MergeDriverPipelineCacheFile(vulkan_device, path);
  if (!merged_size) {
    // Save the pipelines created so far with the first save.
    driver_pipeline_cache_first_unsaved_use_ms_.store(GetDriverPipelineCacheClockMs(),
                                                      std::memory_order_relaxed);
    return;
  }
  driver_pipeline_cache_saved_size_ = merged_size;
  // The built-in pipelines created since the setup merge may have added data (all cache hits on
  // later runs, which leave the size unchanged and skip the save).
  if (command_processor_.setup_driver_pipeline_cache_path() == path) {
    MarkDriverPipelineCacheUsed();
  }
}

void VulkanPipelineCache::MarkDriverPipelineCacheUsed() {
  uint64_t now_ms = GetDriverPipelineCacheClockMs();
  driver_pipeline_cache_last_use_ms_.store(now_ms, std::memory_order_relaxed);
  uint64_t no_unsaved_use = 0;
  driver_pipeline_cache_first_unsaved_use_ms_.compare_exchange_strong(no_unsaved_use, now_ms,
                                                                      std::memory_order_relaxed);
}

int64_t VulkanPipelineCache::GetDriverPipelineCacheSaveDelayMs() const {
  if (driver_pipeline_cache_path_.empty()) {
    return -1;
  }
  uint64_t first_unsaved_use_ms =
      driver_pipeline_cache_first_unsaved_use_ms_.load(std::memory_order_relaxed);
  if (!first_unsaved_use_ms) {
    return -1;
  }
  uint64_t last_use_ms = std::max(
      driver_pipeline_cache_last_use_ms_.load(std::memory_order_relaxed), first_unsaved_use_ms);
  // A few seconds after the last creation, or periodically while creations keep coming. A big
  // cache (precompiled pipeline libraries: well over 100 MB) is rewritten whole by every save, so
  // it waits for a longer quiet period and is saved at most every few minutes.
  bool big = driver_pipeline_cache_saved_size_ > (size_t(32) << 20);
  uint64_t settle_ms = big ? 30000 : kDriverPipelineCacheSaveSettleMs;
  uint64_t max_delay_ms = big ? 180000 : kDriverPipelineCacheSaveMaxDelayMs;
  if (driver_pipeline_cache_save_soon_.load(std::memory_order_relaxed)) {
    settle_ms = kDriverPipelineCacheSaveSettleMs;
    max_delay_ms = kDriverPipelineCacheSaveSettleMs;
  }
  uint64_t save_ms =
      std::min(last_use_ms + settle_ms, first_unsaved_use_ms + max_delay_ms);
  uint64_t now_ms = GetDriverPipelineCacheClockMs();
  return save_ms > now_ms ? int64_t(save_ms - now_ms) : 0;
}

void VulkanPipelineCache::SaveDriverPipelineCache() {
  if (driver_pipeline_cache_path_.empty()) {
    return;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const VkPipelineCache device_pipeline_cache = vulkan_device->pipeline_cache();
  if (device_pipeline_cache == VK_NULL_HANDLE) {
    return;
  }
  std::lock_guard<std::mutex> lock(driver_pipeline_cache_save_mutex_);
  // Creations from now on are for the next save.
  driver_pipeline_cache_first_unsaved_use_ms_.store(0, std::memory_order_relaxed);
  driver_pipeline_cache_save_soon_.store(false, std::memory_order_relaxed);
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // Library keys created before the data is fetched are covered by this save.
  std::vector<uint64_t> saved_library_keys;
  {
    std::lock_guard<std::mutex> libraries_lock(pipeline_libraries_mutex_);
    saved_library_keys.swap(unsaved_library_keys_);
  }
  auto restore_library_keys = [&]() {
    std::lock_guard<std::mutex> libraries_lock(pipeline_libraries_mutex_);
    unsaved_library_keys_.insert(unsaved_library_keys_.end(), saved_library_keys.begin(),
                                 saved_library_keys.end());
  };
  // Getting the data is internally synchronized with pipeline creation by other threads.
  size_t data_size = 0;
  if (dfn.vkGetPipelineCacheData(device, device_pipeline_cache, &data_size, nullptr) !=
          VK_SUCCESS ||
      !data_size) {
    restore_library_keys();
    return;
  }
  if (data_size == driver_pipeline_cache_saved_size_) {
    // Only cache hits since the last load or save.
    restore_library_keys();
    return;
  }
  std::vector<uint8_t> data(data_size);
  VkResult data_result =
      dfn.vkGetPipelineCacheData(device, device_pipeline_cache, &data_size, data.data());
  if (data_result != VK_SUCCESS) {
    if (data_result == VK_INCOMPLETE) {
      // Grew in between - retry on the next round.
      MarkDriverPipelineCacheUsed();
    }
    restore_library_keys();
    return;
  }
  data.resize(data_size);
  std::filesystem::path temp_path = driver_pipeline_cache_path_;
  temp_path += ".tmp";
  bool written = false;
  if (FILE* file = rex::filesystem::OpenFile(temp_path, "wb")) {
    written = fwrite(data.data(), 1, data.size(), file) == data.size();
    written = FlushAndSyncStorageFile(file) && written;
    written = fclose(file) == 0 && written;
  }
  std::error_code error_code;
  if (written) {
    std::filesystem::rename(temp_path, driver_pipeline_cache_path_, error_code);
  }
  if (!written || error_code) {
    std::filesystem::remove(temp_path, error_code);
    REXGPU_WARN("VulkanPipelineCache: Failed to save the driver pipeline cache to {}",
                rex::path_to_utf8(driver_pipeline_cache_path_));
    restore_library_keys();
    return;
  }
  if (library_keys_file_ && !saved_library_keys.empty()) {
    fwrite(saved_library_keys.data(), sizeof(uint64_t), saved_library_keys.size(),
           library_keys_file_);
    fflush(library_keys_file_);
  }
  driver_pipeline_cache_saved_size_ = data.size();
  REXGPU_INFO("VulkanPipelineCache: Saved the driver pipeline cache: {} KB to {}",
              data.size() / 1024, rex::path_to_utf8(driver_pipeline_cache_path_));
}

void VulkanPipelineCache::EndSubmission() {
  if (shader_storage_file_flush_needed_ || pipeline_storage_file_flush_needed_) {
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      if (shader_storage_file_flush_needed_) {
        storage_write_flush_shaders_ = true;
      }
      if (pipeline_storage_file_flush_needed_) {
        storage_write_flush_pipelines_ = true;
      }
    }
    storage_write_request_cond_.notify_one();
    shader_storage_file_flush_needed_ = false;
    pipeline_storage_file_flush_needed_ = false;
  }

  if (!creation_threads_.empty()) {
    bool startup_loading = false;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      startup_loading = startup_loading_;
    }
    if (startup_loading || REXCVAR_GET(vulkan_pipeline_compile_never_blocks)) {
      creation_request_cond_.notify_one();
    } else {
      // Help worker threads on the processor thread to reduce warm-up latency.
      CreateQueuedPipelinesOnProcessorThread();
      bool await_creation_completion_event;
      {
        std::lock_guard<std::mutex> lock(creation_request_lock_);
        // The queue is empty because of CreateQueuedPipelinesOnProcessorThread,
        // only check in-flight creation by worker threads.
        await_creation_completion_event = creation_threads_busy_ != 0;
        if (await_creation_completion_event) {
          creation_completion_event_->Reset();
          creation_completion_set_event_ = true;
        }
      }
      if (await_creation_completion_event) {
        creation_request_cond_.notify_one();
        rex::thread::Wait(creation_completion_event_.get(), false);
      }
    }
  }

  ProcessDeferredPipelineDestructions(false);
}

void VulkanPipelineCache::Shutdown() {
  // Shut down creation threads before destroying any pipelines they may touch.
  if (!creation_threads_.empty()) {
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_threads_shutdown_from_ = 0;
    }
    creation_request_cond_.notify_all();
    for (size_t i = 0; i < creation_threads_.size(); ++i) {
      rex::thread::Wait(creation_threads_[i].get(), false);
    }
    creation_threads_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    while (!creation_queue_.empty()) {
      creation_queue_.pop();
    }
    creation_threads_busy_ = 0;
    startup_loading_ = false;
    creation_completion_set_event_ = false;
    creation_threads_shutdown_from_ = SIZE_MAX;
  }
  creation_completion_event_.reset();

  ShutdownShaderStorage();

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  ProcessDeferredPipelineDestructions(true);

  // Destroy all pipelines.
  last_pipeline_ = nullptr;
  for (const auto& pipeline_pair : pipelines_) {
    VkPipeline pipeline = pipeline_pair.second.pipeline.load(std::memory_order_acquire);
    if (pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
  pipelines_.clear();
  {
    std::lock_guard<std::mutex> lock(pipeline_libraries_mutex_);
    for (const auto& library_pair : pipeline_libraries_) {
      if (library_pair.second != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, library_pair.second, nullptr);
      }
    }
    pipeline_libraries_.clear();
  }

  // Destroy all internal shaders.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         depth_only_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         depth_float24_truncate_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         depth_float24_round_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         placeholder_pixel_shader_);
  if (tessellation_indexed_vertex_shader_ != VK_NULL_HANDLE) {
    dfn.vkDestroyShaderModule(device, tessellation_indexed_vertex_shader_, nullptr);
    tessellation_indexed_vertex_shader_ = VK_NULL_HANDLE;
  }
  if (tessellation_adaptive_vertex_shader_ != VK_NULL_HANDLE) {
    dfn.vkDestroyShaderModule(device, tessellation_adaptive_vertex_shader_, nullptr);
    tessellation_adaptive_vertex_shader_ = VK_NULL_HANDLE;
  }
  tessellation_indexed_vertex_shader_attempted_ = false;
  tessellation_adaptive_vertex_shader_attempted_ = false;
  for (const auto& tessellation_control_shader_pair : tessellation_control_shaders_) {
    if (tessellation_control_shader_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, tessellation_control_shader_pair.second, nullptr);
    }
  }
  tessellation_control_shaders_.clear();
  for (const auto& geometry_shader_pair : geometry_shaders_) {
    if (geometry_shader_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, geometry_shader_pair.second, nullptr);
    }
  }
  geometry_shaders_.clear();

  // Destroy all translated shaders.
  for (auto it : shaders_) {
    delete it.second;
  }
  shaders_.clear();
  shader_storage_index_ = 0;
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();

  // Shut down shader translation.
  shader_translator_.reset();
  async_shader_translator_.reset();
}

VulkanShader* VulkanPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                              const uint32_t* host_address, uint32_t dword_count) {
  // Hash the input memory and lookup the shader.
  return LoadShader(shader_type, host_address, dword_count,
                    XXH3_64bits(host_address, dword_count * sizeof(uint32_t)));
}

VulkanShader* VulkanPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                              const uint32_t* host_address, uint32_t dword_count,
                                              uint64_t data_hash) {
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    // Shader has been previously loaded.
    return it->second;
  }
  // Always create the shader and stash it away.
  // We need to track it even if it fails translation so we know not to try
  // again.
  VulkanShader* shader = new VulkanShader(command_processor_.GetVulkanDevice(), shader_type,
                                          data_hash, host_address, dword_count);
  shaders_.emplace(data_hash, shader);
  return shader;
}

SpirvShaderTranslator::Modification VulkanPipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, bool ps_param_gen_used) const {
  assert_true(shader.type() == xenos::ShaderType::kVertex);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;
  const auto& device_properties = command_processor_.GetVulkanDevice()->properties();

  SpirvShaderTranslator::Modification modification(
      shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;
  if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
    modification.vertex.tessellation_mode = uint32_t(regs.Get<reg::VGT_HOS_CNTL>().tess_mode);
  } else {
    modification.vertex.tessellation_mode = 0;
  }

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  // Without the host's depth clamp, disabled guest clipping is emulated in the vertex shader (the pipeline's
  // depth_clamp_enable needs the feature); a full-screen pass otherwise lost its bottom edge under Mali emulation.
  modification.vertex.clamp_depth = !device_properties.depthClamp && pa_cl_clip_cntl.clip_disable ? 1 : 0;
  uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  if (REXCVAR_GET(gpu_debug_drop_user_clip_planes)) {
    user_clip_planes = 0;
  }
  if (user_clip_planes) {
    if (pa_cl_clip_cntl.ucp_cull_only_ena && device_properties.shaderCullDistance) {
      modification.vertex.user_clip_plane_cull = 1;
    } else if (!device_properties.shaderClipDistance && device_properties.shaderCullDistance) {
      // Fallback if clip distances are unavailable.
      modification.vertex.user_clip_plane_cull = 1;
    } else if (!device_properties.shaderClipDistance) {
      // No supported clip/cull built-in for user clip planes.
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXGPU_WARN("User clip planes (mask {:X}) dropped: no clip or cull distances on this device (logged once)",
                    user_clip_planes);
      }
      user_clip_planes = 0;
      modification.vertex.user_clip_plane_cull = 0;
    } else {
      modification.vertex.user_clip_plane_cull = 0;
    }
  } else {
    modification.vertex.user_clip_plane_cull = 0;
  }
  modification.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  modification.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;

  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = uint32_t(ps_param_gen_used);
  } else {
    modification.vertex.output_point_parameters =
        uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  }
  modification.vertex.vertex_kill_and = uint32_t(
      device_properties.shaderCullDistance &&
      (shader.writes_point_size_edge_flag_kill_vertex() & 0b100) && !pa_cl_clip_cntl.vtx_kill_or);

  return modification;
}

SpirvShaderTranslator::Modification VulkanPipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control) const {
  assert_true(shader.type() == xenos::ShaderType::kPixel);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  SpirvShaderTranslator::Modification modification(
      shader_translator_->GetDefaultPixelShaderModification(
          shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));

  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                              regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                              regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                              regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  if (render_target_cache_.GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
    if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
        normalized_depth_control.z_enable &&
        regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      // For host D32 depth, emulate guest D24FS8 float24 depth behavior.
      modification.pixel.depth_stencil_mode = render_target_cache_.depth_float24_round()
                                                  ? DepthStencilMode::kFloat24Rounding
                                                  : DepthStencilMode::kFloat24Truncating;
    } else {
      if (shader.implicit_early_z_write_allowed() &&
          (!shader.writes_color_target(0) ||
           !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))) {
        modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
      } else {
        modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
      }
    }
  }

  return modification;
}

bool VulkanPipelineCache::EnsureShadersTranslated(VulkanShader::VulkanTranslation* vertex_shader,
                                                  VulkanShader::VulkanTranslation* pixel_shader) {
  // Edge flags are not supported yet (because polygon primitives are not).
  assert_true(register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdge &&
              register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdgeKill);
  assert_false(register_file_.Get<reg::SQ_PROGRAM_CNTL>().gen_index_vtx);
  // The clock is read only when a shader is translated (this runs for every draw).
  bool translated_any = false;
  std::chrono::steady_clock::time_point translation_start;
  struct TranslationCounter {
    VulkanPipelineCache* cache;
    const std::chrono::steady_clock::time_point* start;
    bool* translated_any;
    ~TranslationCounter() {
      if (*translated_any) {
        cache->counter_translations_.fetch_add(1, std::memory_order_relaxed);
        cache->counter_translations_us_.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - *start)
                         .count()),
            std::memory_order_relaxed);
      }
    }
  } translation_counter{this, &translation_start, &translated_any};
  if (!vertex_shader->is_translated()) {
    translated_any = true;
    translation_start = std::chrono::steady_clock::now();
    vertex_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
    if (!TranslateAnalyzedShader(*shader_translator_, *vertex_shader)) {
      REXGPU_ERROR("Failed to translate the vertex shader!");
      return false;
    }
  }
  if (!vertex_shader->is_valid()) {
    // Translation attempted previously, but not valid.
    return false;
  }
  if (pixel_shader != nullptr) {
    if (!pixel_shader->is_translated()) {
      if (!translated_any) {
        translation_start = std::chrono::steady_clock::now();
      }
      translated_any = true;
      pixel_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
      if (!TranslateAnalyzedShader(*shader_translator_, *pixel_shader)) {
        REXGPU_ERROR("Failed to translate the pixel shader!");
        return false;
      }
    }
    if (!pixel_shader->is_valid()) {
      // Translation attempted previously, but not valid.
      return false;
    }
  }
  return true;
}

bool VulkanPipelineCache::ConfigurePipeline(
    VulkanShader::VulkanTranslation* vertex_shader, VulkanShader::VulkanTranslation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    VulkanRenderTargetCache::RenderPassKey render_pass_key, VkPipeline& pipeline_out,
    const PipelineLayoutProvider*& pipeline_layout_out, void** pipeline_handle_out,
    uint32_t texture_sign_classes, uint32_t vertex_texture_sign_classes) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Shader translation takes 10-25 ms each on a phone: when the pipeline is created
  // asynchronously anyway, translate on the creation threads (vulkan_async_shader_translation);
  // the state description only needs the ucode analysis. Otherwise translate here as before.
  const bool never_block = REXCVAR_GET(vulkan_pipeline_compile_never_blocks);
  bool use_async = REXCVAR_GET(async_shader_compilation) && !creation_threads_.empty() &&
                   (never_block || (pixel_shader && placeholder_pixel_shader_ != VK_NULL_HANDLE));
  bool translate_async = false;
  if (use_async && never_block && async_shader_translator_ &&
      REXCVAR_GET(vulkan_async_shader_translation) &&
      (!vertex_shader->is_translated() || (pixel_shader && !pixel_shader->is_translated()))) {
    translate_async = true;
    for (VulkanShader::VulkanTranslation* translation : {vertex_shader, pixel_shader}) {
      if (!translation) {
        continue;
      }
      if (translation->is_translated() && !translation->is_valid()) {
        // Translation attempted previously, but not valid.
        return false;
      }
      if (!translation->shader().is_ucode_analyzed()) {
        translation->shader().AnalyzeUcode(ucode_disasm_buffer_);
      }
    }
  } else if (!EnsureShadersTranslated(vertex_shader, pixel_shader)) {
    return false;
  }
  if (shader_storage_file_ &&
      vertex_shader->shader().ucode_storage_index() != shader_storage_index_) {
    vertex_shader->shader().set_ucode_storage_index(shader_storage_index_);
    assert_not_null(storage_write_thread_);
    shader_storage_file_flush_needed_ = true;
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_shader_queue_.push_back(&vertex_shader->shader());
    }
    storage_write_request_cond_.notify_all();
  }
  if (pixel_shader && shader_storage_file_ &&
      pixel_shader->shader().ucode_storage_index() != shader_storage_index_) {
    pixel_shader->shader().set_ucode_storage_index(shader_storage_index_);
    assert_not_null(storage_write_thread_);
    shader_storage_file_flush_needed_ = true;
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_shader_queue_.push_back(&pixel_shader->shader());
    }
    storage_write_request_cond_.notify_all();
  }

  PipelineDescription description;
  if (!GetCurrentStateDescription(vertex_shader, pixel_shader, primitive_processing_result,
                                  normalized_depth_control, normalized_color_mask, render_pass_key,
                                  description)) {
    REXGPU_ERROR(
        "VulkanPipelineCache: GetCurrentStateDescription failed "
        "(guest_prim={}, host_prim={}, host_vs_type={}, tess_mode={}, host_reset={}, "
        "render_pass_key=0x{:08X})",
        uint32_t(primitive_processing_result.guest_primitive_type),
        uint32_t(primitive_processing_result.host_primitive_type),
        uint32_t(primitive_processing_result.host_vertex_shader_type),
        uint32_t(primitive_processing_result.tessellation_mode),
        uint32_t(primitive_processing_result.host_primitive_reset_enabled), render_pass_key.key);
    return false;
  }
  if (!pixel_shader || !SpirvShaderTranslator::IsTextureSignSpecializationEnabled()) {
    texture_sign_classes = 0;
  }
  // Vertex sign classes have no place in the stored description (see PipelineDescription), so
  // vertex shaders always use the runtime signs (spirv_vertex_texture_sign_specialization measured
  // no gain anyway).
  (void)vertex_texture_sign_classes;
  const bool try_sign_specialization =
      texture_sign_classes && !translate_async && use_async && never_block;
  // The fast path only for the variant that is preferred (with sign classes: the specialized one;
  // while it's pending, the lookups below run and fall back to the runtime variant).
  PipelineDescription preferred_description = description;
  if (try_sign_specialization) {
    preferred_description.texture_sign_classes = texture_sign_classes & 0xFFFF;
    preferred_description.texture_exp_adjust_zero =
        (texture_sign_classes & SpirvShaderTranslator::kTextureSignClassesExpAdjustZero) ? 1 : 0;
  }
  if (last_pipeline_ && last_pipeline_->first == preferred_description) {
    VkPipeline last_pipeline = last_pipeline_->second.pipeline.load(std::memory_order_acquire);
    const PipelineLayoutProvider* last_pipeline_layout =
        last_pipeline_->second.pipeline_layout.load(std::memory_order_acquire);
    if (last_pipeline != VK_NULL_HANDLE && last_pipeline_layout != nullptr) {
      pipeline_out = last_pipeline;
      pipeline_layout_out = last_pipeline_layout;
      if (pipeline_handle_out) {
        *pipeline_handle_out = const_cast<Pipeline*>(&last_pipeline_->second);
      }
      return true;
    }
  }

  auto count_sync_creation = [this](std::chrono::steady_clock::time_point start) {
    counter_pipelines_sync_.fetch_add(1, std::memory_order_relaxed);
    counter_pipelines_sync_us_.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count()),
        std::memory_order_relaxed);
  };
  uint8_t async_priority = pipeline_util::kPriorityLowest;
  if (use_async) {
    uint32_t bound_rts =
        pipeline_util::GetBoundRTMaskFromNormalizedColorMask(normalized_color_mask);
    uint32_t shader_writes_color_targets =
        pixel_shader ? pixel_shader->shader().writes_color_targets() : 0;
    bool shader_writes_depth = pixel_shader ? pixel_shader->shader().writes_depth()
                                            : normalized_depth_control.z_write_enable != 0;
    async_priority = pipeline_util::CalculatePipelinePriority(
        bound_rts, shader_writes_color_targets, shader_writes_depth);
  }

  // Texture sign specialization: use the variant with the bound textures' sign classes when it's
  // ready; otherwise queue it on the creation threads (or link it right here from cached
  // libraries) and draw with the runtime variant meanwhile, so it never holds a draw.
  if (try_sign_specialization) {
    const PipelineDescription& specialized_description = preferred_description;
    auto use_specialized = [&](std::pair<const PipelineDescription, Pipeline>& entry) {
      VkPipeline entry_pipeline = entry.second.pipeline.load(std::memory_order_acquire);
      const PipelineLayoutProvider* entry_layout =
          entry.second.pipeline_layout.load(std::memory_order_acquire);
      if (entry_pipeline == VK_NULL_HANDLE || entry_layout == nullptr) {
        return false;
      }
      last_pipeline_ = &entry;
      pipeline_out = entry_pipeline;
      pipeline_layout_out = entry_layout;
      if (pipeline_handle_out) {
        *pipeline_handle_out = &entry.second;
      }
      counter_sign_specialized_draws_.fetch_add(1, std::memory_order_relaxed);
      return true;
    };
    auto specialized_it = pipelines_.find(specialized_description);
    if (specialized_it != pipelines_.end()) {
      // Pending or failed: the runtime variant below.
      if (use_specialized(*specialized_it)) {
        return true;
      }
    } else {
      auto& specialized = *pipelines_.emplace(specialized_description, Pipeline()).first;
      PipelineCreationArguments specialized_arguments;
      // On failure the entry stays with a null pipeline (not a placeholder): the runtime variant
      // is used for this state from now on.
      if (TryGetPipelineCreationArgumentsForDescription(specialized_description, &specialized,
                                                        specialized_arguments)) {
        bool linked = uses_pipeline_libraries_ &&
                      EnsurePipelineCreatedFromLibraries(specialized_arguments, true);
        if (!linked) {
          specialized_arguments.priority = pipeline_util::kPriorityLowest;
          specialized.second.is_placeholder.store(true, std::memory_order_release);
          {
            std::lock_guard<std::mutex> lock(creation_request_lock_);
            creation_queue_.push(specialized_arguments);
          }
          creation_request_cond_.notify_one();
          counter_pipelines_queued_.fetch_add(1, std::memory_order_relaxed);
        }
        counter_sign_specialized_created_.fetch_add(1, std::memory_order_relaxed);
        if (pipeline_storage_file_) {
          assert_not_null(storage_write_thread_);
          pipeline_storage_file_flush_needed_ = true;
          {
            std::lock_guard<std::mutex> lock(storage_write_request_lock_);
            storage_write_pipeline_queue_.emplace_back();
            PipelineStoredDescription& stored_description = storage_write_pipeline_queue_.back();
            stored_description.description_hash = specialized_description.GetHash();
            std::memcpy(&stored_description.description, &specialized_description,
                        sizeof(specialized_description));
          }
          storage_write_request_cond_.notify_all();
        }
        if (linked && use_specialized(specialized)) {
          return true;
        }
      }
    }
    counter_sign_runtime_draws_.fetch_add(1, std::memory_order_relaxed);
  }

  auto it = pipelines_.find(description);
  if (it != pipelines_.end()) {
    VkPipeline found_pipeline = it->second.pipeline.load(std::memory_order_acquire);
    if (found_pipeline == VK_NULL_HANDLE) {
      if (it->second.is_placeholder.load(std::memory_order_acquire)) {
        // Still being created by the background threads: the caller skips the draw.
        pipeline_out = VK_NULL_HANDLE;
        pipeline_layout_out = nullptr;
        if (pipeline_handle_out) {
          *pipeline_handle_out = &it->second;
        }
        return true;
      }
      PipelineCreationArguments creation_arguments;
      auto sync_start = std::chrono::steady_clock::now();
      if (!TryGetPipelineCreationArgumentsForDescription(description, &*it, creation_arguments) ||
          !EnsurePipelineCreated(creation_arguments)) {
        return false;
      }
      count_sync_creation(sync_start);
      found_pipeline = it->second.pipeline.load(std::memory_order_acquire);
    }
    const PipelineLayoutProvider* found_pipeline_layout =
        it->second.pipeline_layout.load(std::memory_order_acquire);
    last_pipeline_ = &*it;
    pipeline_out = found_pipeline;
    pipeline_layout_out = found_pipeline_layout;
    if (pipeline_handle_out) {
      *pipeline_handle_out = &it->second;
    }
    return pipeline_out != VK_NULL_HANDLE && pipeline_layout_out != nullptr;
  }

  // Create the pipeline if not already existing.
  auto& pipeline = *pipelines_.emplace(description, Pipeline()).first;
  PipelineCreationArguments creation_arguments_real;
  if (!TryGetPipelineCreationArgumentsForDescription(description, &pipeline,
                                                     creation_arguments_real, false,
                                                     translate_async)) {
    pipelines_.erase(description);
    return false;
  }

  bool queued_async_creation = false;
  bool linked_from_cached_libraries = false;
  if (uses_pipeline_libraries_ && !translate_async) {
    // Every library already exists or is a driver cache hit: a fast link right here, no pending
    // draws and no held frame.
    auto link_start = std::chrono::steady_clock::now();
    if (EnsurePipelineCreatedFromLibraries(creation_arguments_real, true)) {
      linked_from_cached_libraries = true;
      counter_pipelines_linked_sync_.fetch_add(1, std::memory_order_relaxed);
      counter_pipelines_linked_sync_us_.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - link_start)
                       .count()),
          std::memory_order_relaxed);
    }
  }
  if (linked_from_cached_libraries) {
    // Published by EnsurePipelineCreatedFromLibraries.
  } else if (use_async && never_block) {
    // Pending until a creation thread publishes it (is_placeholder with a null pipeline).
    creation_arguments_real.priority = async_priority;
    pipeline.second.is_placeholder.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_queue_.push(creation_arguments_real);
    }
    creation_request_cond_.notify_one();
    queued_async_creation = true;
    counter_pipelines_queued_.fetch_add(1, std::memory_order_relaxed);
  } else if (use_async) {
    creation_arguments_real.priority = async_priority;
    PipelineCreationArguments creation_arguments_placeholder;
    if (TryGetPipelineCreationArgumentsForDescription(description, &pipeline,
                                                      creation_arguments_placeholder, true)) {
      pipeline.second.is_placeholder.store(true, std::memory_order_release);
      if (EnsurePipelineCreated(creation_arguments_placeholder, placeholder_pixel_shader_)) {
        {
          std::lock_guard<std::mutex> lock(creation_request_lock_);
          creation_queue_.push(creation_arguments_real);
        }
        creation_request_cond_.notify_one();
        queued_async_creation = true;
      } else {
        pipeline.second.is_placeholder.store(false, std::memory_order_release);
      }
    }
  }

  if (!queued_async_creation && !linked_from_cached_libraries) {
    auto sync_start = std::chrono::steady_clock::now();
    if (!EnsurePipelineCreated(creation_arguments_real)) {
      pipelines_.erase(description);
      return false;
    }
    count_sync_creation(sync_start);
  }
  if (!queued_async_creation) {
    pipeline.second.is_placeholder.store(false, std::memory_order_release);
  }

  if (pipeline_storage_file_) {
    assert_not_null(storage_write_thread_);
    pipeline_storage_file_flush_needed_ = true;
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_pipeline_queue_.emplace_back();
      PipelineStoredDescription& stored_description = storage_write_pipeline_queue_.back();
      stored_description.description_hash = description.GetHash();
      std::memcpy(&stored_description.description, &description, sizeof(description));
    }
    storage_write_request_cond_.notify_all();
  }
  last_pipeline_ = &pipeline;
  pipeline_out = pipeline.second.pipeline.load(std::memory_order_acquire);
  pipeline_layout_out = pipeline.second.pipeline_layout.load(std::memory_order_acquire);
  if (pipeline_handle_out) {
    *pipeline_handle_out = &pipeline.second;
  }
  if (pipeline_out == VK_NULL_HANDLE && queued_async_creation &&
      pipeline.second.is_placeholder.load(std::memory_order_acquire)) {
    // Pending: the caller skips the draw.
    return true;
  }
  return pipeline_out != VK_NULL_HANDLE && pipeline_layout_out != nullptr;
}

VulkanPipelineCache::DrawTimeCounters VulkanPipelineCache::TakeDrawTimeCounters() {
  DrawTimeCounters counters;
  counters.pipelines_queued = counter_pipelines_queued_.exchange(0, std::memory_order_relaxed);
  counters.pipelines_sync = counter_pipelines_sync_.exchange(0, std::memory_order_relaxed);
  counters.pipelines_sync_us = counter_pipelines_sync_us_.exchange(0, std::memory_order_relaxed);
  counters.translations = counter_translations_.exchange(0, std::memory_order_relaxed);
  counters.translations_us = counter_translations_us_.exchange(0, std::memory_order_relaxed);
  counters.translations_async = counter_translations_async_.exchange(0, std::memory_order_relaxed);
  counters.translations_async_us =
      counter_translations_async_us_.exchange(0, std::memory_order_relaxed);
  counters.libraries_created = counter_libraries_created_.exchange(0, std::memory_order_relaxed);
  counters.libraries_created_us =
      counter_libraries_created_us_.exchange(0, std::memory_order_relaxed);
  counters.pipelines_linked = counter_pipelines_linked_.exchange(0, std::memory_order_relaxed);
  counters.pipelines_linked_us =
      counter_pipelines_linked_us_.exchange(0, std::memory_order_relaxed);
  counters.pipelines_linked_sync =
      counter_pipelines_linked_sync_.exchange(0, std::memory_order_relaxed);
  counters.pipelines_linked_sync_us =
      counter_pipelines_linked_sync_us_.exchange(0, std::memory_order_relaxed);
  counters.sign_specialized_draws =
      counter_sign_specialized_draws_.exchange(0, std::memory_order_relaxed);
  counters.sign_runtime_draws = counter_sign_runtime_draws_.exchange(0, std::memory_order_relaxed);
  counters.sign_specialized_created =
      counter_sign_specialized_created_.exchange(0, std::memory_order_relaxed);
  return counters;
}

const VulkanPipelineCache::PipelineDynamicState* VulkanPipelineCache::GetPipelineDynamicState(
    void* handle) const {
  if (!handle) {
    return nullptr;
  }
  const auto* pipeline = reinterpret_cast<const Pipeline*>(handle);
  return pipeline->uses_dynamic_state.load(std::memory_order_acquire) ? &pipeline->dynamic_state
                                                                       : nullptr;
}

bool VulkanPipelineCache::TranslatesShadersOnCreationThreads() const {
  return REXCVAR_GET(async_shader_compilation) && !creation_threads_.empty() &&
         REXCVAR_GET(vulkan_pipeline_compile_never_blocks) && async_shader_translator_ &&
         REXCVAR_GET(vulkan_async_shader_translation);
}

bool VulkanPipelineCache::IsCreatingPipelines() const {
  std::lock_guard<std::mutex> lock(creation_request_lock_);
  if (creation_threads_.empty()) {
    return startup_loading_;
  }
  return startup_loading_ || !creation_queue_.empty() || creation_threads_busy_ != 0;
}

void VulkanPipelineCache::GetPipelineAndLayoutByHandle(
    void* handle, VkPipeline& pipeline_out, const PipelineLayoutProvider*& pipeline_layout_out,
    bool* is_placeholder_out) const {
  if (!handle) {
    pipeline_out = VK_NULL_HANDLE;
    pipeline_layout_out = nullptr;
    if (is_placeholder_out) {
      *is_placeholder_out = false;
    }
    return;
  }
  const auto* pipeline = reinterpret_cast<const Pipeline*>(handle);
  pipeline_out = pipeline->pipeline.load(std::memory_order_acquire);
  pipeline_layout_out = pipeline->pipeline_layout.load(std::memory_order_acquire);
  if (is_placeholder_out) {
    *is_placeholder_out = pipeline->is_placeholder.load(std::memory_order_acquire);
  }
}

bool VulkanPipelineCache::TranslateAnalyzedShader(SpirvShaderTranslator& translator,
                                                  VulkanShader::VulkanTranslation& translation) {
  VulkanShader& shader = static_cast<VulkanShader&>(translation.shader());

  // Perform translation.
  // If this fails the shader will be marked as invalid and ignored later.
  if (!translator.TranslateAnalyzedShader(translation)) {
    REXGPU_ERROR("Shader {:016X} translation failed; marking as ignored", shader.ucode_data_hash());
    return false;
  }
  if (translation.GetOrCreateShaderModule() == VK_NULL_HANDLE) {
    return false;
  }
  // Dump the SPIR-V (shader_<ucode hash>_<modification>.vk.bin.vert/frag) like
  // the D3D12 backend does, if desired.
  if (!REXCVAR_GET(dump_shaders).empty()) {
    translation.Dump(REXCVAR_GET(dump_shaders), "vk");
  }

  // TODO(Triang3l): Log that the shader has been successfully translated in
  // common code.

  // Set up the texture binding layout.
  if (shader.EnterBindingLayoutUserUIDSetup()) {
    std::lock_guard<std::mutex> texture_binding_layouts_lock(texture_binding_layouts_mutex_);
    // Obtain the unique IDs of the binding layout if there are any texture
    // bindings, for invalidation in the command processor.
    size_t texture_binding_layout_uid = kLayoutUIDEmpty;
    const std::vector<VulkanShader::TextureBinding>& texture_bindings =
        shader.GetTextureBindingsAfterTranslation();
    size_t texture_binding_count = texture_bindings.size();
    if (texture_binding_count) {
      size_t texture_binding_layout_bytes =
          texture_binding_count * sizeof(*texture_bindings.data());
      uint64_t texture_binding_layout_hash =
          XXH3_64bits(texture_bindings.data(), texture_binding_layout_bytes);
      auto found_range = texture_binding_layout_map_.equal_range(texture_binding_layout_hash);
      for (auto it = found_range.first; it != found_range.second; ++it) {
        if (it->second.vector_span_length == texture_binding_count &&
            !std::memcmp(texture_binding_layouts_.data() + it->second.vector_span_offset,
                         texture_bindings.data(), texture_binding_layout_bytes)) {
          texture_binding_layout_uid = it->second.uid;
          break;
        }
      }
      if (texture_binding_layout_uid == kLayoutUIDEmpty) {
        static_assert(kLayoutUIDEmpty == 0,
                      "Layout UID is size + 1 because it's assumed that 0 is the UID for "
                      "an empty layout");
        texture_binding_layout_uid = texture_binding_layout_map_.size() + 1;
        LayoutUID new_uid;
        new_uid.uid = texture_binding_layout_uid;
        new_uid.vector_span_offset = texture_binding_layouts_.size();
        new_uid.vector_span_length = texture_binding_count;
        texture_binding_layouts_.resize(new_uid.vector_span_offset + texture_binding_count);
        std::memcpy(texture_binding_layouts_.data() + new_uid.vector_span_offset,
                    texture_bindings.data(), texture_binding_layout_bytes);
        texture_binding_layout_map_.emplace(texture_binding_layout_hash, new_uid);
      }
    }
    shader.SetTextureBindingLayoutUserUID(texture_binding_layout_uid);

    // Use the sampler count for samplers because it's the only thing that must
    // be the same for layouts to be compatible in this case
    // (instruction-specified parameters are used as overrides for creating
    // actual samplers).
    static_assert(kLayoutUIDEmpty == 0,
                  "Empty layout UID is assumed to be 0 because for bindful samplers, the "
                  "UID is their count");
    shader.SetSamplerBindingLayoutUserUID(shader.GetSamplerBindingsAfterTranslation().size());
  }

  return true;
}

void VulkanPipelineCache::WritePipelineRenderTargetDescription(
    reg::RB_BLENDCONTROL blend_control, uint32_t write_mask,
    PipelineRenderTarget& render_target_out) const {
  if (write_mask) {
    assert_zero(write_mask & ~uint32_t(0b1111));
    // 32 because of 0x1F mask, for safety (all unknown to zero).
    static const PipelineBlendFactor kBlendFactorMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcColor,
        /*  5 */ PipelineBlendFactor::kOneMinusSrcColor,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kOneMinusSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDstColor,
        /*  9 */ PipelineBlendFactor::kOneMinusDstColor,
        /* 10 */ PipelineBlendFactor::kDstAlpha,
        /* 11 */ PipelineBlendFactor::kOneMinusDstAlpha,
        /* 12 */ PipelineBlendFactor::kConstantColor,
        /* 13 */ PipelineBlendFactor::kOneMinusConstantColor,
        /* 14 */ PipelineBlendFactor::kConstantAlpha,
        /* 15 */ PipelineBlendFactor::kOneMinusConstantAlpha,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSaturate,
    };
    render_target_out.src_color_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.color_srcblend)];
    render_target_out.dst_color_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.color_destblend)];
    render_target_out.color_blend_op = blend_control.color_comb_fcn;
    render_target_out.src_alpha_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.alpha_srcblend)];
    render_target_out.dst_alpha_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.alpha_destblend)];
    render_target_out.alpha_blend_op = blend_control.alpha_comb_fcn;
    if (!command_processor_.GetVulkanDevice()->properties().constantAlphaColorBlendFactors) {
      if (blend_control.color_srcblend == xenos::BlendFactor::kConstantAlpha) {
        render_target_out.src_color_blend_factor = PipelineBlendFactor::kConstantColor;
      } else if (blend_control.color_srcblend == xenos::BlendFactor::kOneMinusConstantAlpha) {
        render_target_out.src_color_blend_factor = PipelineBlendFactor::kOneMinusConstantColor;
      }
      if (blend_control.color_destblend == xenos::BlendFactor::kConstantAlpha) {
        render_target_out.dst_color_blend_factor = PipelineBlendFactor::kConstantColor;
      } else if (blend_control.color_destblend == xenos::BlendFactor::kOneMinusConstantAlpha) {
        render_target_out.dst_color_blend_factor = PipelineBlendFactor::kOneMinusConstantColor;
      }
    }
  } else {
    render_target_out.src_color_blend_factor = PipelineBlendFactor::kOne;
    render_target_out.dst_color_blend_factor = PipelineBlendFactor::kZero;
    render_target_out.color_blend_op = xenos::BlendOp::kAdd;
    render_target_out.src_alpha_blend_factor = PipelineBlendFactor::kOne;
    render_target_out.dst_alpha_blend_factor = PipelineBlendFactor::kZero;
    render_target_out.alpha_blend_op = xenos::BlendOp::kAdd;
  }
  render_target_out.color_write_mask = write_mask;
}

bool VulkanPipelineCache::GetCurrentStateDescription(
    const VulkanShader::VulkanTranslation* vertex_shader,
    const VulkanShader::VulkanTranslation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    VulkanRenderTargetCache::RenderPassKey render_pass_key,
    PipelineDescription& description_out) const {
  description_out.Reset();

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();

  const RegisterFile& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();

  description_out.vertex_shader_hash = vertex_shader->shader().ucode_data_hash();
  description_out.vertex_shader_modification = vertex_shader->modification();
  SpirvShaderTranslator::Modification vertex_shader_modification(
      description_out.vertex_shader_modification);
  Shader::HostVertexShaderType host_vertex_shader_type =
      vertex_shader_modification.vertex.host_vertex_shader_type;
  if (pixel_shader) {
    description_out.pixel_shader_hash = pixel_shader->shader().ucode_data_hash();
    description_out.pixel_shader_modification = pixel_shader->modification();
    SpirvShaderTranslator::Modification pixel_shader_modification(
        description_out.pixel_shader_modification);
    using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
    description_out.sample_rate_shading =
        !pixel_shader->shader().writes_depth() &&
        (pixel_shader_modification.pixel.depth_stencil_mode ==
             DepthStencilMode::kFloat24Truncating ||
         pixel_shader_modification.pixel.depth_stencil_mode == DepthStencilMode::kFloat24Rounding);
  }
  description_out.render_pass_key = render_pass_key;

  bool tessellated = primitive_processing_result.IsTessellated();
  PipelineGeometryShader geometry_shader = PipelineGeometryShader::kNone;
  PipelinePrimitiveTopology primitive_topology;
  if (tessellated) {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kQuadList:
      case xenos::PrimitiveType::kTrianglePatch:
      case xenos::PrimitiveType::kQuadPatch:
        primitive_topology = PipelinePrimitiveTopology::kPatchList;
        break;
      default:
        return false;
    }
    description_out.tessellation_mode = primitive_processing_result.tessellation_mode;
  } else {
    // Fallback expansion in the vertex shader for primitive types that may be
    // emulated through geometry shaders on GS-capable hosts.
    if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
        host_vertex_shader_type == Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
      if (primitive_processing_result.host_primitive_type != xenos::PrimitiveType::kTriangleStrip ||
          !primitive_processing_result.host_primitive_reset_enabled) {
        assert_always();
        return false;
      }
      primitive_topology = PipelinePrimitiveTopology::kTriangleStrip;
    } else {
      switch (primitive_processing_result.host_primitive_type) {
        case xenos::PrimitiveType::kPointList:
          geometry_shader = PipelineGeometryShader::kPointList;
          primitive_topology = PipelinePrimitiveTopology::kPointList;
          break;
        case xenos::PrimitiveType::kLineList:
          primitive_topology = PipelinePrimitiveTopology::kLineList;
          break;
        case xenos::PrimitiveType::kLineStrip:
          primitive_topology = PipelinePrimitiveTopology::kLineStrip;
          break;
        case xenos::PrimitiveType::kTriangleList:
          primitive_topology = PipelinePrimitiveTopology::kTriangleList;
          break;
        case xenos::PrimitiveType::kTriangleFan:
          // Triangle fans on Vulkan are expected to be converted to triangle
          // lists in primitive processing for parity with D3D12 behavior.
          assert_always();
          return false;
        case xenos::PrimitiveType::kTriangleStrip:
          primitive_topology = PipelinePrimitiveTopology::kTriangleStrip;
          break;
        case xenos::PrimitiveType::kRectangleList:
          geometry_shader = PipelineGeometryShader::kRectangleList;
          primitive_topology = PipelinePrimitiveTopology::kTriangleList;
          break;
        case xenos::PrimitiveType::kQuadList:
          geometry_shader = PipelineGeometryShader::kQuadList;
          primitive_topology = PipelinePrimitiveTopology::kLineListWithAdjacency;
          break;
        default:
          return false;
      }
    }
  }
  description_out.geometry_shader = geometry_shader;
  description_out.primitive_topology = primitive_topology;
  description_out.primitive_restart = primitive_processing_result.host_primitive_reset_enabled;

  description_out.depth_clamp_enable =
      device_properties.depthClamp && regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;

  // Tessellated draws are patch-domain polygonal primitives regardless of guest
  // register ambiguity in non-explicit major mode configurations.
  bool primitive_polygonal = tessellated ? true : draw_util::IsPrimitivePolygonal(regs);
  bool rasterization_enabled = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (!rasterization_enabled) {
    // Keep parity with D3D12 by fully disabling rasterization for draws where
    // only non-raster stages (for instance, vertex memexport) can have side
    // effects.
    description_out.rasterizer_discard = 1;
    return true;
  }
  if (primitive_polygonal) {
    // Vulkan only allows the polygon mode to be set for both faces - pick the
    // most special one (more likely to represent the developer's deliberate
    // intentions - fill is very generic, wireframe is common in debug, points
    // are for pretty unusual things, but closer to debug purposes too - on the
    // Xenos, points have the lowest register value and triangles have the
    // highest) based on which faces are not culled.
    bool cull_front = pa_su_sc_mode_cntl.cull_front;
    bool cull_back = pa_su_sc_mode_cntl.cull_back;
    description_out.cull_front = cull_front;
    description_out.cull_back = cull_back;
    if (device_properties.fillModeNonSolid) {
      xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
      if (!cull_front) {
        polygon_type = std::min(polygon_type, pa_su_sc_mode_cntl.polymode_front_ptype);
      }
      if (!cull_back) {
        polygon_type = std::min(polygon_type, pa_su_sc_mode_cntl.polymode_back_ptype);
      }
      if (pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDualMode) {
        polygon_type = xenos::PolygonType::kTriangles;
      }
      switch (polygon_type) {
        case xenos::PolygonType::kPoints:
        case xenos::PolygonType::kLines:
          // Keep non-triangle polygon fill behavior aligned with D3D12 (which
          // maps both point and line polygon modes to wireframe).
          description_out.polygon_mode = PipelinePolygonMode::kLine;
          break;
        case xenos::PolygonType::kTriangles:
          description_out.polygon_mode = PipelinePolygonMode::kFill;
          break;
        default:
          assert_unhandled_case(polygon_type);
          return false;
      }
    } else {
      description_out.polygon_mode = PipelinePolygonMode::kFill;
    }
    description_out.front_face_clockwise = pa_su_sc_mode_cntl.face != 0;
  } else {
    description_out.polygon_mode = PipelinePolygonMode::kFill;
  }
  if (tessellated && REXCVAR_GET(vulkan_tessellation_wireframe) &&
      device_properties.fillModeNonSolid) {
    description_out.polygon_mode = PipelinePolygonMode::kLine;
  }

  if (render_target_cache_.GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    if (render_pass_key.depth_and_color_used & 1) {
      if (normalized_depth_control.z_enable) {
        description_out.depth_write_enable = normalized_depth_control.z_write_enable;
        description_out.depth_compare_op = normalized_depth_control.zfunc;
      } else {
        description_out.depth_compare_op = xenos::CompareFunction::kAlways;
      }
      if (normalized_depth_control.stencil_enable) {
        description_out.stencil_test_enable = 1;
        description_out.stencil_front_fail_op = normalized_depth_control.stencilfail;
        description_out.stencil_front_pass_op = normalized_depth_control.stencilzpass;
        description_out.stencil_front_depth_fail_op = normalized_depth_control.stencilzfail;
        description_out.stencil_front_compare_op = normalized_depth_control.stencilfunc;
        if (primitive_polygonal && normalized_depth_control.backface_enable) {
          description_out.stencil_back_fail_op = normalized_depth_control.stencilfail_bf;
          description_out.stencil_back_pass_op = normalized_depth_control.stencilzpass_bf;
          description_out.stencil_back_depth_fail_op = normalized_depth_control.stencilzfail_bf;
          description_out.stencil_back_compare_op = normalized_depth_control.stencilfunc_bf;
        } else {
          description_out.stencil_back_fail_op = description_out.stencil_front_fail_op;
          description_out.stencil_back_pass_op = description_out.stencil_front_pass_op;
          description_out.stencil_back_depth_fail_op = description_out.stencil_front_depth_fail_op;
          description_out.stencil_back_compare_op = description_out.stencil_front_compare_op;
        }
      }
    }

    // Color blending and write masks (filled only for the attachments present
    // in the render pass object).
    uint32_t render_pass_color_rts = render_pass_key.depth_and_color_used >> 1;
    assert_true(device_properties.independentBlend);
    uint32_t render_pass_color_rts_remaining = render_pass_color_rts;
    uint32_t color_rt_index;
    while (rex::bit_scan_forward(render_pass_color_rts_remaining, &color_rt_index)) {
      render_pass_color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
      WritePipelineRenderTargetDescription(
          regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[color_rt_index]),
          (normalized_color_mask >> (color_rt_index * 4)) & 0b1111,
          description_out.render_targets[color_rt_index]);
    }
  }

  return true;
}

bool VulkanPipelineCache::ArePipelineRequirementsMet(const PipelineDescription& description) const {
  VkShaderStageFlags vertex_shader_stage =
      Shader::IsHostVertexShaderTypeDomain(
          SpirvShaderTranslator::Modification(description.vertex_shader_modification)
              .vertex.host_vertex_shader_type)
          ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
          : VK_SHADER_STAGE_VERTEX_BIT;
  if (!(guest_shader_vertex_stages_ & vertex_shader_stage)) {
    return false;
  }

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();

  if (!device_properties.geometryShader &&
      description.geometry_shader != PipelineGeometryShader::kNone) {
    return false;
  }

  // Stored pipelines made on another GPU (the shader cache bundled with the Android app comes from an Adreno) may
  // clip or cull with distances this device lacks (Arm's Mali driver has neither); such a draw is made again with
  // what the device has when it comes.
  {
    const SpirvShaderTranslator::Modification vertex_modification(description.vertex_shader_modification);
    if (vertex_modification.vertex.user_clip_plane_count &&
        !(vertex_modification.vertex.user_clip_plane_cull ? device_properties.shaderCullDistance
                                                          : device_properties.shaderClipDistance)) {
      return false;
    }
  }

  // Keep fan handling consistent with D3D12 by always using primitive
  // processor conversion to triangle lists instead of native fan topologies.
  if (description.primitive_topology == PipelinePrimitiveTopology::kTriangleFan) {
    return false;
  }

  if (!device_properties.depthClamp && description.depth_clamp_enable) {
    return false;
  }

  if (!device_properties.pointPolygons && description.polygon_mode == PipelinePolygonMode::kPoint) {
    return false;
  }

  if (!device_properties.fillModeNonSolid &&
      description.polygon_mode != PipelinePolygonMode::kFill) {
    return false;
  }

  assert_true(device_properties.independentBlend);

  if (!device_properties.constantAlphaColorBlendFactors) {
    uint32_t color_rts_remaining = description.render_pass_key.depth_and_color_used >> 1;
    uint32_t color_rt_index;
    while (rex::bit_scan_forward(color_rts_remaining, &color_rt_index)) {
      color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
      const PipelineRenderTarget& color_rt = description.render_targets[color_rt_index];
      if (color_rt.src_color_blend_factor == PipelineBlendFactor::kConstantAlpha ||
          color_rt.src_color_blend_factor == PipelineBlendFactor::kOneMinusConstantAlpha ||
          color_rt.dst_color_blend_factor == PipelineBlendFactor::kConstantAlpha ||
          color_rt.dst_color_blend_factor == PipelineBlendFactor::kOneMinusConstantAlpha) {
        return false;
      }
    }
  }

  return true;
}

uint32_t VulkanPipelineCache::GetTessellationPatchControlPointCount(
    Shader::HostVertexShaderType host_vertex_shader_type,
    xenos::TessellationMode tessellation_mode) {
  switch (tessellation_mode) {
    case xenos::TessellationMode::kDiscrete:
    case xenos::TessellationMode::kContinuous:
      switch (host_vertex_shader_type) {
        case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
          return 3;
        case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
          return 1;
        case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
          return 4;
        case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
          return 1;
        default:
          return 0;
      }
    case xenos::TessellationMode::kAdaptive:
      switch (host_vertex_shader_type) {
        case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
          return 3;
        case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
          return 4;
        default:
          return 0;
      }
    default:
      return 0;
  }
}

VkShaderModule VulkanPipelineCache::GetTessellationVertexShader(bool adaptive) {
  bool& attempted = adaptive ? tessellation_adaptive_vertex_shader_attempted_
                             : tessellation_indexed_vertex_shader_attempted_;
  VkShaderModule& shader_module =
      adaptive ? tessellation_adaptive_vertex_shader_ : tessellation_indexed_vertex_shader_;
  if (attempted) {
    return shader_module;
  }
  attempted = true;

  std::vector<uint32_t> shader_spirv;
  std::string compile_error;
  std::string source = adaptive ? GetTessellationAdaptiveVertexShaderGlsl()
                                : GetTessellationIndexedVertexShaderGlsl();
  if (!command_processor_.CompileGlslToSpirv(VK_SHADER_STAGE_VERTEX_BIT, source, shader_spirv,
                                             compile_error)) {
    REXGPU_ERROR(
        "VulkanPipelineCache: Failed to compile tessellation {} vertex "
        "shader: {}",
        adaptive ? "adaptive" : "indexed", compile_error);
    shader_module = VK_NULL_HANDLE;
    return shader_module;
  }

  shader_module = ui::vulkan::util::CreateShaderModule(command_processor_.GetVulkanDevice(),
                                                       shader_spirv.data(),
                                                       sizeof(uint32_t) * shader_spirv.size());
  if (shader_module == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "VulkanPipelineCache: Failed to create tessellation {} vertex "
        "shader module",
        adaptive ? "adaptive" : "indexed");
  }
  return shader_module;
}

VkShaderModule VulkanPipelineCache::GetTessellationControlShader(
    Shader::HostVertexShaderType host_vertex_shader_type,
    xenos::TessellationMode tessellation_mode) {
  TessellationControlShaderKey key;
  key.host_vertex_shader_type = host_vertex_shader_type;
  key.tessellation_mode = tessellation_mode;
  auto it = tessellation_control_shaders_.find(key);
  if (it != tessellation_control_shaders_.end()) {
    return it->second;
  }

  uint32_t input_control_points =
      GetTessellationPatchControlPointCount(host_vertex_shader_type, tessellation_mode);
  uint32_t output_control_points = 0;
  if (tessellation_mode == xenos::TessellationMode::kAdaptive) {
    output_control_points = 1;
  } else if (IsPatchIndexedHostVertexShaderType(host_vertex_shader_type)) {
    output_control_points = 1;
  } else if (IsTriangleDomainHostVertexShaderType(host_vertex_shader_type)) {
    output_control_points = 3;
  } else if (IsQuadDomainHostVertexShaderType(host_vertex_shader_type)) {
    output_control_points = 4;
  }
  if (!input_control_points || !output_control_points) {
    tessellation_control_shaders_.emplace(key, VK_NULL_HANDLE);
    return VK_NULL_HANDLE;
  }

  std::vector<uint32_t> shader_spirv;
  std::string compile_error;
  std::string source = GetTessellationControlShaderGlsl(
      host_vertex_shader_type, tessellation_mode, input_control_points, output_control_points);
  if (!command_processor_.CompileGlslToSpirv(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, source,
                                             shader_spirv, compile_error)) {
    REXGPU_ERROR(
        "VulkanPipelineCache: Failed to compile tessellation control shader "
        "0x{:08X}: {}",
        key.key, compile_error);
    tessellation_control_shaders_.emplace(key, VK_NULL_HANDLE);
    return VK_NULL_HANDLE;
  }

  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      command_processor_.GetVulkanDevice(), shader_spirv.data(),
      sizeof(uint32_t) * shader_spirv.size());
  if (shader_module == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "VulkanPipelineCache: Failed to create tessellation control shader "
        "module 0x{:08X}",
        key.key);
  }
  tessellation_control_shaders_.emplace(key, shader_module);
  return shader_module;
}

bool VulkanPipelineCache::GetGeometryShaderKey(
    PipelineGeometryShader geometry_shader_type,
    SpirvShaderTranslator::Modification vertex_shader_modification,
    SpirvShaderTranslator::Modification pixel_shader_modification, GeometryShaderKey& key_out) {
  if (geometry_shader_type == PipelineGeometryShader::kNone) {
    return false;
  }
  // For kPointListAsTriangleStrip, output_point_parameters has a different
  // meaning (the coordinates, not the size). However, the AsTriangleStrip host
  // vertex shader types are needed specifically when geometry shaders are not
  // supported as fallbacks.
  if (vertex_shader_modification.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      vertex_shader_modification.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
    assert_always();
    return false;
  }
  GeometryShaderKey key;
  key.type = geometry_shader_type;
  key.interpolator_mask =
      SpirvShaderTranslator::fixed_interpolator_locations()
          ? vertex_shader_modification.vertex.interpolator_mask
          : (UINT32_C(1) << rex::bit_count(vertex_shader_modification.vertex.interpolator_mask)) -
                1;
  key.user_clip_plane_count = vertex_shader_modification.vertex.user_clip_plane_count;
  key.user_clip_plane_cull = vertex_shader_modification.vertex.user_clip_plane_cull;
  key.has_vertex_kill_and = vertex_shader_modification.vertex.vertex_kill_and;
  key.has_point_size = vertex_shader_modification.vertex.output_point_parameters;
  key.has_point_coordinates = pixel_shader_modification.pixel.param_gen_point;
  key.point_ps_ucp_mode = vertex_shader_modification.vertex.point_ps_ucp_mode;
  key_out = key;
  return true;
}

VkShaderModule VulkanPipelineCache::GetGeometryShader(GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key);
  if (it != geometry_shaders_.end()) {
    return it->second;
  }

  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;

  spv::ExecutionMode input_primitive_execution_mode = spv::ExecutionMode(0);
  uint32_t input_primitive_vertex_count = 0;
  spv::ExecutionMode output_primitive_execution_mode = spv::ExecutionMode(0);
  uint32_t output_max_vertices = 0;
  switch (key.type) {
    case PipelineGeometryShader::kPointList:
      // Point to a strip of 2 triangles.
      input_primitive_execution_mode = spv::ExecutionModeInputPoints;
      input_primitive_vertex_count = 1;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case PipelineGeometryShader::kRectangleList:
      // Triangle to a strip of 2 triangles.
      input_primitive_execution_mode = spv::ExecutionModeTriangles;
      input_primitive_vertex_count = 3;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case PipelineGeometryShader::kQuadList:
      // 4 vertices passed via a line list with adjacency to a strip of 2
      // triangles.
      input_primitive_execution_mode = spv::ExecutionModeInputLinesAdjacency;
      input_primitive_vertex_count = 4;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    default:
      assert_unhandled_case(key.type);
  }

  uint32_t clip_distance_count = key.user_clip_plane_cull ? 0 : key.user_clip_plane_count;
  uint32_t point_user_cull_distance_count =
      key.type == PipelineGeometryShader::kPointList && key.user_clip_plane_cull
          ? key.user_clip_plane_count
          : 0;
  uint32_t cull_distance_count = point_user_cull_distance_count + key.has_vertex_kill_and;
  bool point_recalculate_clip_distances = key.type == PipelineGeometryShader::kPointList &&
                                          clip_distance_count && key.point_ps_ucp_mode >= 2;
  bool point_recalculate_cull_distances = key.type == PipelineGeometryShader::kPointList &&
                                          point_user_cull_distance_count &&
                                          key.point_ps_ucp_mode >= 3;

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();
  const ui::vulkan::VulkanDevice::Extensions& device_extensions = vulkan_device->extensions();
  spv::SpvBuildLogger builder_logger;
  spv::SpvBuildLogger* builder_logger_ptr = nullptr;
  spv::SpvVersion spirv_version;
  if (device_properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    spirv_version = spv::Spv_1_5;
  } else if (device_extensions.ext_1_2_KHR_spirv_1_4) {
    spirv_version = spv::Spv_1_4;
  } else if (device_properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    spirv_version = spv::Spv_1_3;
  } else {
    spirv_version = spv::Spv_1_0;
    // Keep the build log around for compatibility diagnostics on older paths.
    builder_logger_ptr = &builder_logger;
  }
  SpirvBuilder builder(spirv_version, (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1,
                       builder_logger_ptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityGeometry);
  if (clip_distance_count) {
    builder.addCapability(spv::CapabilityClipDistance);
  }
  if (cull_distance_count) {
    builder.addCapability(spv::CapabilityCullDistance);
  }
  bool denorm_flush_to_zero_float32 = device_properties.shaderDenormFlushToZeroFloat32;
  bool signed_zero_inf_nan_preserve_float32 =
      device_properties.shaderSignedZeroInfNanPreserveFloat32;
  bool rounding_mode_rte_float32 = device_properties.shaderRoundingModeRTEFloat32;
  if (spirv_version < spv::Spv_1_4) {
    if (denorm_flush_to_zero_float32 || signed_zero_inf_nan_preserve_float32 ||
        rounding_mode_rte_float32) {
      builder.addExtension("SPV_KHR_float_controls");
    }
  }
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  std::vector<spv::Id> main_interface;

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_bool4 = builder.makeVectorType(type_bool, 4);
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float2 = builder.makeVectorType(type_float, 2);
  spv::Id type_float3 = builder.makeVectorType(type_float, 3);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);
  spv::Id type_float4_array_6 =
      builder.makeArrayType(type_float4, builder.makeUintConstant(6), sizeof(float) * 4);
  builder.addDecoration(type_float4_array_6, spv::DecorationArrayStride, sizeof(float) * 4);
  spv::Id type_clip_distances =
      clip_distance_count
          ? builder.makeArrayType(type_float, builder.makeUintConstant(clip_distance_count), 0)
          : spv::NoType;
  spv::Id type_cull_distances =
      cull_distance_count
          ? builder.makeArrayType(type_float, builder.makeUintConstant(cull_distance_count), 0)
          : spv::NoType;

  // System constants.
  // For points:
  // - float2 point_constant_diameter
  // - float2 point_screen_diameter_to_ndc_radius
  // - float3 ndc_scale
  // - float3 ndc_offset
  // - float4[6] user_clip_planes
  enum PointConstant : uint32_t {
    kPointConstantConstantDiameter,
    kPointConstantScreenDiameterToNdcRadius,
    kPointConstantNdcScale,
    kPointConstantNdcOffset,
    kPointConstantUserClipPlanes,
    kPointConstantCount,
  };
  spv::Id type_system_constants = spv::NoType;
  if (key.type == PipelineGeometryShader::kPointList) {
    id_vector_temp.clear();
    id_vector_temp.resize(kPointConstantCount);
    id_vector_temp[kPointConstantConstantDiameter] = type_float2;
    id_vector_temp[kPointConstantScreenDiameterToNdcRadius] = type_float2;
    id_vector_temp[kPointConstantNdcScale] = type_float3;
    id_vector_temp[kPointConstantNdcOffset] = type_float3;
    id_vector_temp[kPointConstantUserClipPlanes] = type_float4_array_6;
    type_system_constants = builder.makeStructType(id_vector_temp, "XeSystemConstants");
    builder.addMemberName(type_system_constants, kPointConstantConstantDiameter,
                          "point_constant_diameter");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantConstantDiameter, spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants, point_constant_diameter)));
    builder.addMemberName(type_system_constants, kPointConstantScreenDiameterToNdcRadius,
                          "point_screen_diameter_to_ndc_radius");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantScreenDiameterToNdcRadius, spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants, point_screen_diameter_to_ndc_radius)));
    builder.addMemberName(type_system_constants, kPointConstantNdcScale, "ndc_scale");
    builder.addMemberDecoration(type_system_constants, kPointConstantNdcScale,
                                spv::DecorationOffset,
                                int(offsetof(SpirvShaderTranslator::SystemConstants, ndc_scale)));
    builder.addMemberName(type_system_constants, kPointConstantNdcOffset, "ndc_offset");
    builder.addMemberDecoration(type_system_constants, kPointConstantNdcOffset,
                                spv::DecorationOffset,
                                int(offsetof(SpirvShaderTranslator::SystemConstants, ndc_offset)));
    builder.addMemberName(type_system_constants, kPointConstantUserClipPlanes, "user_clip_planes");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantUserClipPlanes, spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants, user_clip_planes)));
  }
  spv::Id uniform_system_constants = spv::NoResult;
  if (type_system_constants != spv::NoType) {
    builder.addDecoration(type_system_constants, spv::DecorationBlock);
    uniform_system_constants =
        builder.createVariable(spv::NoPrecision, spv::StorageClassUniform, type_system_constants,
                               "xe_uniform_system_constants");
    builder.addDecoration(uniform_system_constants, spv::DecorationDescriptorSet,
                          int(SpirvShaderTranslator::kDescriptorSetConstants));
    builder.addDecoration(uniform_system_constants, spv::DecorationBinding,
                          int(SpirvShaderTranslator::kConstantBufferSystem));
    main_interface.push_back(uniform_system_constants);
  }

  // Inputs and outputs - matching glslang order, in gl_PerVertex gl_in[],
  // user-defined outputs, user-defined inputs, out gl_PerVertex.

  spv::Id const_input_primitive_vertex_count =
      builder.makeUintConstant(input_primitive_vertex_count);

  // in gl_PerVertex gl_in[].
  // gl_Position.
  id_vector_temp.clear();
  uint32_t member_in_gl_per_vertex_position = uint32_t(id_vector_temp.size());
  id_vector_temp.push_back(type_float4);
  spv::Id const_member_in_gl_per_vertex_position =
      builder.makeIntConstant(int32_t(member_in_gl_per_vertex_position));
  // gl_ClipDistance.
  uint32_t member_in_gl_per_vertex_clip_distance = UINT32_MAX;
  spv::Id const_member_in_gl_per_vertex_clip_distance = spv::NoResult;
  if (clip_distance_count) {
    member_in_gl_per_vertex_clip_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_clip_distances);
    const_member_in_gl_per_vertex_clip_distance =
        builder.makeIntConstant(int32_t(member_in_gl_per_vertex_clip_distance));
  }
  // gl_CullDistance.
  uint32_t member_in_gl_per_vertex_cull_distance = UINT32_MAX;
  if (cull_distance_count) {
    member_in_gl_per_vertex_cull_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_cull_distances);
  }
  // Structure and array.
  spv::Id type_struct_in_gl_per_vertex = builder.makeStructType(id_vector_temp, "gl_PerVertex");
  builder.addMemberName(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_position,
                        "gl_Position");
  builder.addMemberDecoration(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_position,
                              spv::DecorationBuiltIn, spv::BuiltInPosition);
  if (clip_distance_count) {
    builder.addMemberName(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_clip_distance,
                          "gl_ClipDistance");
    builder.addMemberDecoration(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_clip_distance,
                                spv::DecorationBuiltIn, spv::BuiltInClipDistance);
  }
  if (cull_distance_count) {
    builder.addMemberName(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_cull_distance,
                          "gl_CullDistance");
    builder.addMemberDecoration(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_cull_distance,
                                spv::DecorationBuiltIn, spv::BuiltInCullDistance);
  }
  builder.addDecoration(type_struct_in_gl_per_vertex, spv::DecorationBlock);
  spv::Id type_array_in_gl_per_vertex =
      builder.makeArrayType(type_struct_in_gl_per_vertex, const_input_primitive_vertex_count, 0);
  spv::Id in_gl_per_vertex = builder.createVariable(spv::NoPrecision, spv::StorageClassInput,
                                                    type_array_in_gl_per_vertex, "gl_in");
  main_interface.push_back(in_gl_per_vertex);

  uint32_t output_location = 0;

  // Compact interpolator index -> location (the index of the mask's bit).
  const uint32_t interpolator_count = rex::bit_count(uint32_t(key.interpolator_mask));
  std::array<uint32_t, xenos::kMaxInterpolators> interpolator_locations{};
  {
    uint32_t interpolators_remaining = key.interpolator_mask;
    uint32_t interpolator_location;
    uint32_t compact_index = 0;
    while (rex::bit_scan_forward(interpolators_remaining, &interpolator_location)) {
      interpolators_remaining &= ~(UINT32_C(1) << interpolator_location);
      interpolator_locations[compact_index++] = interpolator_location;
    }
  }
  const uint32_t after_interpolators_location =
      SpirvShaderTranslator::fixed_interpolator_locations() ? xenos::kMaxInterpolators
                                                            : interpolator_count;

  // Interpolators outputs.
  std::array<spv::Id, xenos::kMaxInterpolators> out_interpolators;
  for (uint32_t i = 0; i < interpolator_count; ++i) {
    spv::Id out_interpolator =
        builder.createVariable(spv::NoPrecision, spv::StorageClassOutput, type_float4,
                               fmt::format("xe_out_interpolator_{}", i).c_str());
    out_interpolators[i] = out_interpolator;
    builder.addDecoration(out_interpolator, spv::DecorationLocation,
                          int(interpolator_locations[i]));
    builder.addDecoration(out_interpolator, spv::DecorationInvariant);
    main_interface.push_back(out_interpolator);
  }
  output_location = after_interpolators_location;

  // Point coordinate output.
  spv::Id out_point_coordinates = spv::NoResult;
  if (key.has_point_coordinates) {
    out_point_coordinates = builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                   type_float2, "xe_out_point_coordinates");
    builder.addDecoration(out_point_coordinates, spv::DecorationLocation, int(output_location));
    builder.addDecoration(out_point_coordinates, spv::DecorationInvariant);
    main_interface.push_back(out_point_coordinates);
    ++output_location;
  }

  uint32_t input_location = 0;

  // Interpolator inputs.
  std::array<spv::Id, xenos::kMaxInterpolators> in_interpolators;
  for (uint32_t i = 0; i < interpolator_count; ++i) {
    spv::Id in_interpolator = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(type_float4, const_input_primitive_vertex_count, 0),
        fmt::format("xe_in_interpolator_{}", i).c_str());
    in_interpolators[i] = in_interpolator;
    builder.addDecoration(in_interpolator, spv::DecorationLocation,
                          int(interpolator_locations[i]));
    main_interface.push_back(in_interpolator);
  }
  input_location = after_interpolators_location;

  // Point size input.
  spv::Id in_point_size = spv::NoResult;
  if (key.has_point_size) {
    in_point_size = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(type_float, const_input_primitive_vertex_count, 0),
        "xe_in_point_size");
    builder.addDecoration(in_point_size, spv::DecorationLocation, int(input_location));
    main_interface.push_back(in_point_size);
    ++input_location;
  }

  // out gl_PerVertex.
  // gl_Position.
  id_vector_temp.clear();
  uint32_t member_out_gl_per_vertex_position = uint32_t(id_vector_temp.size());
  id_vector_temp.push_back(type_float4);
  spv::Id const_member_out_gl_per_vertex_position =
      builder.makeIntConstant(int32_t(member_out_gl_per_vertex_position));
  // gl_ClipDistance.
  uint32_t member_out_gl_per_vertex_clip_distance = UINT32_MAX;
  spv::Id const_member_out_gl_per_vertex_clip_distance = spv::NoResult;
  if (clip_distance_count) {
    member_out_gl_per_vertex_clip_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_clip_distances);
    const_member_out_gl_per_vertex_clip_distance =
        builder.makeIntConstant(int32_t(member_out_gl_per_vertex_clip_distance));
  }
  // Structure.
  spv::Id type_struct_out_gl_per_vertex = builder.makeStructType(id_vector_temp, "gl_PerVertex");
  builder.addMemberName(type_struct_out_gl_per_vertex, member_out_gl_per_vertex_position,
                        "gl_Position");
  builder.addMemberDecoration(type_struct_out_gl_per_vertex, member_out_gl_per_vertex_position,
                              spv::DecorationBuiltIn, spv::BuiltInPosition);
  if (clip_distance_count) {
    builder.addMemberName(type_struct_out_gl_per_vertex, member_out_gl_per_vertex_clip_distance,
                          "gl_ClipDistance");
    builder.addMemberDecoration(type_struct_out_gl_per_vertex,
                                member_out_gl_per_vertex_clip_distance, spv::DecorationBuiltIn,
                                spv::BuiltInClipDistance);
  }
  builder.addDecoration(type_struct_out_gl_per_vertex, spv::DecorationBlock);
  spv::Id out_gl_per_vertex = builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                     type_struct_out_gl_per_vertex, "");
  builder.addDecoration(out_gl_per_vertex, spv::DecorationInvariant);
  main_interface.push_back(out_gl_per_vertex);

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function = builder.makeFunctionEntry(
      spv::NoPrecision, type_void, "main", main_param_types, main_precisions, &main_entry);
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelGeometry, main_function, "main");
  for (spv::Id interface_id : main_interface) {
    entry_point->addIdOperand(interface_id);
  }
  builder.addExecutionMode(main_function, input_primitive_execution_mode);
  builder.addExecutionMode(main_function, spv::ExecutionModeInvocations, 1);
  builder.addExecutionMode(main_function, output_primitive_execution_mode);
  builder.addExecutionMode(main_function, spv::ExecutionModeOutputVertices,
                           int(output_max_vertices));
  if (denorm_flush_to_zero_float32) {
    builder.addCapability(spv::CapabilityDenormFlushToZero);
    builder.addExecutionMode(main_function, spv::ExecutionModeDenormFlushToZero, 32);
  }
  if (signed_zero_inf_nan_preserve_float32) {
    builder.addCapability(spv::CapabilitySignedZeroInfNanPreserve);
    builder.addExecutionMode(main_function, spv::ExecutionModeSignedZeroInfNanPreserve, 32);
  }
  if (rounding_mode_rte_float32) {
    builder.addCapability(spv::CapabilityRoundingModeRTE);
    builder.addExecutionMode(main_function, spv::ExecutionModeRoundingModeRTE, 32);
  }

  // Note that after every OpEmitVertex, all output variables are undefined.

  // Discard the whole primitive if any vertex has a NaN position (may also be
  // set to NaN for emulation of vertex killing with the OR operator).
  for (uint32_t i = 0; i < input_primitive_vertex_count; ++i) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
    id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
    spv::Id position_is_nan = builder.createUnaryOp(
        spv::OpAny, type_bool,
        builder.createUnaryOp(
            spv::OpIsNan, type_bool4,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision)));
    spv::Block& discard_predecessor = *builder.getBuildPoint();
    spv::Block& discard_then_block = builder.makeNewBlock();
    spv::Block& discard_merge_block = builder.makeNewBlock();
    builder.createSelectionMerge(&discard_merge_block, spv::SelectionControlDontFlattenMask);
    {
      std::unique_ptr<spv::Instruction> branch_conditional_op(
          std::make_unique<spv::Instruction>(spv::OpBranchConditional));
      branch_conditional_op->addIdOperand(position_is_nan);
      branch_conditional_op->addIdOperand(discard_then_block.getId());
      branch_conditional_op->addIdOperand(discard_merge_block.getId());
      branch_conditional_op->addImmediateOperand(1);
      branch_conditional_op->addImmediateOperand(2);
      discard_predecessor.addInstruction(std::move(branch_conditional_op));
    }
    discard_then_block.addPredecessor(&discard_predecessor);
    discard_merge_block.addPredecessor(&discard_predecessor);
    builder.setBuildPoint(&discard_then_block);
    builder.createNoResultOp(spv::OpReturn);
    builder.setBuildPoint(&discard_merge_block);
  }

  // Cull the whole primitive if any cull distance for all vertices in the
  // primitive is < 0.
  // For point lists with ps_ucp_mode 3, user cull plane distances are
  // calculated per expanded vertex later.
  if (cull_distance_count) {
    spv::Id const_member_in_gl_per_vertex_cull_distance =
        builder.makeIntConstant(int32_t(member_in_gl_per_vertex_cull_distance));
    spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
    spv::Id cull_condition = spv::NoResult;
    uint32_t cull_distance_start =
        point_recalculate_cull_distances ? point_user_cull_distance_count : 0;
    if (cull_distance_start < cull_distance_count) {
      for (uint32_t i = cull_distance_start; i < cull_distance_count; ++i) {
        for (uint32_t j = 0; j < input_primitive_vertex_count; ++j) {
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeIntConstant(int32_t(j)));
          id_vector_temp.push_back(const_member_in_gl_per_vertex_cull_distance);
          id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
          spv::Id cull_distance_is_negative = builder.createBinOp(
              spv::OpFOrdLessThan, type_bool,
              builder.createLoad(builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                                           id_vector_temp),
                                 spv::NoPrecision),
              const_float_0);
          if (cull_condition != spv::NoResult) {
            cull_condition = builder.createBinOp(spv::OpLogicalAnd, type_bool, cull_condition,
                                                 cull_distance_is_negative);
          } else {
            cull_condition = cull_distance_is_negative;
          }
        }
      }
      assert_true(cull_condition != spv::NoResult);
      spv::Block& discard_predecessor = *builder.getBuildPoint();
      spv::Block& discard_then_block = builder.makeNewBlock();
      spv::Block& discard_merge_block = builder.makeNewBlock();
      builder.createSelectionMerge(&discard_merge_block, spv::SelectionControlDontFlattenMask);
      {
        std::unique_ptr<spv::Instruction> branch_conditional_op(
            std::make_unique<spv::Instruction>(spv::OpBranchConditional));
        branch_conditional_op->addIdOperand(cull_condition);
        branch_conditional_op->addIdOperand(discard_then_block.getId());
        branch_conditional_op->addIdOperand(discard_merge_block.getId());
        branch_conditional_op->addImmediateOperand(1);
        branch_conditional_op->addImmediateOperand(2);
        discard_predecessor.addInstruction(std::move(branch_conditional_op));
      }
      discard_then_block.addPredecessor(&discard_predecessor);
      discard_merge_block.addPredecessor(&discard_predecessor);
      builder.setBuildPoint(&discard_then_block);
      builder.createNoResultOp(spv::OpReturn);
      builder.setBuildPoint(&discard_merge_block);
    }
  }

  switch (key.type) {
    case PipelineGeometryShader::kPointList: {
      // Expand the point sprite, with left-to-right, top-to-bottom UVs.

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_float_0 = builder.makeFloatConstant(0.0f);

      // Load the point diameter in guest pixels.
      id_vector_temp.clear();
      id_vector_temp.push_back(builder.makeIntConstant(int32_t(kPointConstantConstantDiameter)));
      id_vector_temp.push_back(const_int_0);
      spv::Id point_guest_diameter_x =
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision);
      id_vector_temp.back() = const_int_1;
      spv::Id point_guest_diameter_y =
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision);
      if (key.has_point_size) {
        // The vertex shader's header writes -1.0 to point_size by default, so
        // any non-negative value means that it was overwritten by the
        // translated vertex shader, and needs to be used instead of the
        // constant size. The per-vertex diameter is already clamped in the
        // vertex shader (combined with making it non-negative).
        id_vector_temp.clear();
        // 0 is the input primitive vertex index.
        id_vector_temp.push_back(const_int_0);
        spv::Id point_vertex_diameter = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_point_size, id_vector_temp),
            spv::NoPrecision);
        spv::Id point_vertex_diameter_written = builder.createBinOp(
            spv::OpFOrdGreaterThanEqual, type_bool, point_vertex_diameter, const_float_0);
        point_guest_diameter_x =
            builder.createTriOp(spv::OpSelect, type_float, point_vertex_diameter_written,
                                point_vertex_diameter, point_guest_diameter_x);
        point_guest_diameter_y =
            builder.createTriOp(spv::OpSelect, type_float, point_vertex_diameter_written,
                                point_vertex_diameter, point_guest_diameter_y);
      }

      // 4D5307F1 has zero-size snowflakes, drop them quicker, and also drop
      // points with a constant size of zero since point lists may also be used
      // as just "compute" with memexport.
      spv::Id point_size_not_zero =
          builder.createBinOp(spv::OpLogicalAnd, type_bool,
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  point_guest_diameter_x, const_float_0),
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  point_guest_diameter_y, const_float_0));
      spv::Block& point_size_zero_predecessor = *builder.getBuildPoint();
      spv::Block& point_size_zero_then_block = builder.makeNewBlock();
      spv::Block& point_size_zero_merge_block = builder.makeNewBlock();
      builder.createSelectionMerge(&point_size_zero_merge_block,
                                   spv::SelectionControlDontFlattenMask);
      {
        std::unique_ptr<spv::Instruction> branch_conditional_op(
            std::make_unique<spv::Instruction>(spv::OpBranchConditional));
        branch_conditional_op->addIdOperand(point_size_not_zero);
        branch_conditional_op->addIdOperand(point_size_zero_merge_block.getId());
        branch_conditional_op->addIdOperand(point_size_zero_then_block.getId());
        branch_conditional_op->addImmediateOperand(2);
        branch_conditional_op->addImmediateOperand(1);
        point_size_zero_predecessor.addInstruction(std::move(branch_conditional_op));
      }
      point_size_zero_then_block.addPredecessor(&point_size_zero_predecessor);
      point_size_zero_merge_block.addPredecessor(&point_size_zero_predecessor);
      builder.setBuildPoint(&point_size_zero_then_block);
      builder.createNoResultOp(spv::OpReturn);
      builder.setBuildPoint(&point_size_zero_merge_block);

      // Transform the diameter in the guest screen coordinates to radius in the
      // normalized device coordinates, and then to the clip space by
      // multiplying by W.
      id_vector_temp.clear();
      id_vector_temp.push_back(
          builder.makeIntConstant(int32_t(kPointConstantScreenDiameterToNdcRadius)));
      id_vector_temp.push_back(const_int_0);
      spv::Id point_radius_x = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_guest_diameter_x,
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision));
      id_vector_temp.back() = const_int_1;
      spv::Id point_radius_y = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_guest_diameter_y,
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision));
      id_vector_temp.clear();
      // 0 is the input primitive vertex index.
      id_vector_temp.push_back(const_int_0);
      id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
      spv::Id point_position = builder.createLoad(
          builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
          spv::NoPrecision);
      spv::Id point_w = builder.createCompositeExtract(point_position, type_float, 3);
      point_radius_x =
          builder.createNoContractionBinOp(spv::OpFMul, type_float, point_radius_x, point_w);
      point_radius_y =
          builder.createNoContractionBinOp(spv::OpFMul, type_float, point_radius_y, point_w);

      // Load the inputs for the guest point.
      // Interpolators.
      std::array<spv::Id, xenos::kMaxInterpolators> point_interpolators;
      id_vector_temp.clear();
      // 0 is the input primitive vertex index.
      id_vector_temp.push_back(const_int_0);
      for (uint32_t i = 0; i < interpolator_count; ++i) {
        point_interpolators[i] = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_interpolators[i], id_vector_temp),
            spv::NoPrecision);
      }
      // Positions.
      spv::Id point_x = builder.createCompositeExtract(point_position, type_float, 0);
      spv::Id point_y = builder.createCompositeExtract(point_position, type_float, 1);
      std::array<spv::Id, 2> point_edge_x, point_edge_y;
      for (uint32_t i = 0; i < 2; ++i) {
        spv::Op point_radius_add_op = i ? spv::OpFAdd : spv::OpFSub;
        point_edge_x[i] = builder.createNoContractionBinOp(point_radius_add_op, type_float, point_x,
                                                           point_radius_x);
        point_edge_y[i] = builder.createNoContractionBinOp(point_radius_add_op, type_float, point_y,
                                                           point_radius_y);
      };
      spv::Id point_z = builder.createCompositeExtract(point_position, type_float, 2);
      // Clip distances.
      spv::Id point_clip_distances = spv::NoResult;
      std::vector<spv::Id> point_user_clip_planes;
      point_user_clip_planes.reserve(clip_distance_count);
      spv::Id point_ndc_scale = spv::NoResult;
      spv::Id point_ndc_offset = spv::NoResult;
      if (point_recalculate_clip_distances || point_recalculate_cull_distances) {
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.makeIntConstant(int32_t(kPointConstantNdcScale)));
        point_ndc_scale =
            builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                         uniform_system_constants, id_vector_temp),
                               spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.makeIntConstant(int32_t(kPointConstantNdcOffset)));
        point_ndc_offset =
            builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                         uniform_system_constants, id_vector_temp),
                               spv::NoPrecision);
        uint32_t point_user_clip_plane_count =
            point_recalculate_cull_distances ? point_user_cull_distance_count : clip_distance_count;
        for (uint32_t i = 0; i < point_user_clip_plane_count; ++i) {
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeIntConstant(int32_t(kPointConstantUserClipPlanes)));
          id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
          point_user_clip_planes.push_back(builder.createLoad(
              builder.createAccessChain(spv::StorageClassUniform, uniform_system_constants,
                                        id_vector_temp),
              spv::NoPrecision));
        }
      } else if (clip_distance_count) {
        id_vector_temp.clear();
        // 0 is the input primitive vertex index.
        id_vector_temp.push_back(const_int_0);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        point_clip_distances = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
      }

      if (point_recalculate_cull_distances) {
        // Cull if any user clip plane has all expanded vertices outside.
        std::array<spv::Id, 4> point_guest_positions;
        for (uint32_t i = 0; i < 4; ++i) {
          uint32_t point_vertex_x = i >> 1;
          uint32_t point_vertex_y = i & 1;
          id_vector_temp.clear();
          id_vector_temp.push_back(point_edge_x[point_vertex_x]);
          id_vector_temp.push_back(point_edge_y[point_vertex_y]);
          id_vector_temp.push_back(point_z);
          id_vector_temp.push_back(point_w);
          spv::Id point_vertex_position =
              builder.createCompositeConstruct(type_float4, id_vector_temp);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 2));
          spv::Id point_vertex_position_xyz =
              builder.createCompositeConstruct(type_float3, id_vector_temp);
          spv::Id point_guest_position_xyz = builder.createNoContractionBinOp(
              spv::OpFSub, type_float3, point_vertex_position_xyz,
              builder.createNoContractionBinOp(spv::OpVectorTimesScalar, type_float3,
                                               point_ndc_offset, point_w));
          point_guest_position_xyz = builder.createNoContractionBinOp(
              spv::OpFDiv, type_float3, point_guest_position_xyz, point_ndc_scale);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 2));
          id_vector_temp.push_back(point_w);
          point_guest_positions[i] = builder.createCompositeConstruct(type_float4, id_vector_temp);
        }
        spv::Id point_cull_condition = spv::NoResult;
        for (uint32_t i = 0; i < point_user_cull_distance_count; ++i) {
          spv::Id point_plane_cull_condition = spv::NoResult;
          for (uint32_t j = 0; j < 4; ++j) {
            spv::Id point_distance_is_negative = builder.createBinOp(
                spv::OpFOrdLessThan, type_bool,
                builder.createBinOp(spv::OpDot, type_float, point_guest_positions[j],
                                    point_user_clip_planes[i]),
                const_float_0);
            if (point_plane_cull_condition != spv::NoResult) {
              point_plane_cull_condition =
                  builder.createBinOp(spv::OpLogicalAnd, type_bool, point_plane_cull_condition,
                                      point_distance_is_negative);
            } else {
              point_plane_cull_condition = point_distance_is_negative;
            }
          }
          if (point_cull_condition != spv::NoResult) {
            point_cull_condition = builder.createBinOp(
                spv::OpLogicalOr, type_bool, point_cull_condition, point_plane_cull_condition);
          } else {
            point_cull_condition = point_plane_cull_condition;
          }
        }
        assert_true(point_cull_condition != spv::NoResult);
        spv::Block& point_cull_predecessor = *builder.getBuildPoint();
        spv::Block& point_cull_then_block = builder.makeNewBlock();
        spv::Block& point_cull_merge_block = builder.makeNewBlock();
        builder.createSelectionMerge(&point_cull_merge_block, spv::SelectionControlDontFlattenMask);
        {
          std::unique_ptr<spv::Instruction> branch_conditional_op(
              std::make_unique<spv::Instruction>(spv::OpBranchConditional));
          branch_conditional_op->addIdOperand(point_cull_condition);
          branch_conditional_op->addIdOperand(point_cull_then_block.getId());
          branch_conditional_op->addIdOperand(point_cull_merge_block.getId());
          branch_conditional_op->addImmediateOperand(1);
          branch_conditional_op->addImmediateOperand(2);
          point_cull_predecessor.addInstruction(std::move(branch_conditional_op));
        }
        point_cull_then_block.addPredecessor(&point_cull_predecessor);
        point_cull_merge_block.addPredecessor(&point_cull_predecessor);
        builder.setBuildPoint(&point_cull_then_block);
        builder.createNoResultOp(spv::OpReturn);
        builder.setBuildPoint(&point_cull_merge_block);
      }

      for (uint32_t i = 0; i < 4; ++i) {
        // Same interpolators for the entire sprite.
        for (uint32_t j = 0; j < interpolator_count; ++j) {
          builder.createStore(point_interpolators[j], out_interpolators[j]);
        }
        // Top-left, bottom-left, top-right, bottom-right order (chosen
        // arbitrarily, simply based on counterclockwise meaning front with
        // frontFace = VkFrontFace(0), but faceness is ignored for non-polygon
        // primitive types).
        uint32_t point_vertex_x = i >> 1;
        uint32_t point_vertex_y = i & 1;
        // Point coordinates.
        if (key.has_point_coordinates) {
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeFloatConstant(float(point_vertex_x)));
          id_vector_temp.push_back(builder.makeFloatConstant(float(point_vertex_y)));
          builder.createStore(builder.makeCompositeConstant(type_float2, id_vector_temp),
                              out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(point_edge_x[point_vertex_x]);
        id_vector_temp.push_back(point_edge_y[point_vertex_y]);
        id_vector_temp.push_back(point_z);
        id_vector_temp.push_back(point_w);
        spv::Id point_vertex_position =
            builder.createCompositeConstruct(type_float4, id_vector_temp);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            point_vertex_position,
            builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (point_recalculate_clip_distances) {
          // Convert host clip space back to guest clip space before applying
          // user clip planes.
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 2));
          spv::Id point_vertex_position_xyz =
              builder.createCompositeConstruct(type_float3, id_vector_temp);
          spv::Id point_guest_position_xyz = builder.createNoContractionBinOp(
              spv::OpFSub, type_float3, point_vertex_position_xyz,
              builder.createNoContractionBinOp(spv::OpVectorTimesScalar, type_float3,
                                               point_ndc_offset, point_w));
          point_guest_position_xyz = builder.createNoContractionBinOp(
              spv::OpFDiv, type_float3, point_guest_position_xyz, point_ndc_scale);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 2));
          id_vector_temp.push_back(point_w);
          spv::Id point_guest_position =
              builder.createCompositeConstruct(type_float4, id_vector_temp);
          for (uint32_t j = 0; j < clip_distance_count; ++j) {
            id_vector_temp.clear();
            id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
            id_vector_temp.push_back(builder.makeIntConstant(int32_t(j)));
            builder.createStore(builder.createBinOp(spv::OpDot, type_float, point_guest_position,
                                                    point_user_clip_planes[j]),
                                builder.createAccessChain(spv::StorageClassOutput,
                                                          out_gl_per_vertex, id_vector_temp));
          }
        } else if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(point_clip_distances,
                              builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex,
                                                        id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case PipelineGeometryShader::kRectangleList: {
      // Construct a strip with the fourth vertex generated by mirroring a
      // vertex across the longest edge (the diagonal).
      //
      // Possible options:
      //
      // 0---1
      // |  /|
      // | / |  - 12 is the longest edge, strip 0123 (most commonly used)
      // |/  |    v3 = v0 + (v1 - v0) + (v2 - v0), or v3 = -v0 + v1 + v2
      // 2--[3]
      //
      // 1---2
      // |  /|
      // | / |  - 20 is the longest edge, strip 1203
      // |/  |
      // 0--[3]
      //
      // 2---0
      // |  /|
      // | / |  - 01 is the longest edge, strip 2013
      // |/  |
      // 1--[3]

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_int_2 = builder.makeIntConstant(2);
      spv::Id const_int_3 = builder.makeIntConstant(3);

      // Get squares of edge lengths to choose the longest edge.
      // [0] - 12, [1] - 20, [2] - 01.
      spv::Id edge_lengths[3];
      id_vector_temp.resize(3);
      id_vector_temp[1] = const_member_in_gl_per_vertex_position;
      for (uint32_t i = 0; i < 3; ++i) {
        id_vector_temp[0] = builder.makeIntConstant(int32_t((1 + i) % 3));
        id_vector_temp[2] = const_int_0;
        spv::Id edge_0_x = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[2] = const_int_1;
        spv::Id edge_0_y = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = builder.makeIntConstant(int32_t((2 + i) % 3));
        id_vector_temp[2] = const_int_0;
        spv::Id edge_1_x = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[2] = const_int_1;
        spv::Id edge_1_y = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        spv::Id edge_x = builder.createBinOp(spv::OpFSub, type_float, edge_1_x, edge_0_x);
        spv::Id edge_y = builder.createBinOp(spv::OpFSub, type_float, edge_1_y, edge_0_y);
        edge_lengths[i] = builder.createBinOp(
            spv::OpFAdd, type_float, builder.createBinOp(spv::OpFMul, type_float, edge_x, edge_x),
            builder.createBinOp(spv::OpFMul, type_float, edge_y, edge_y));
      }

      // Choose the index of the first vertex in the strip based on which edge
      // is the longest, and calculate the indices of the other vertices.
      spv::Id vertex_indices[3];
      // If 12 > 20 && 12 > 01, then 12 is the longest edge, and the strip is
      // 0123. Otherwise, if 20 > 01, then 20 is the longest, and the strip is
      // 1203, but if not, 01 is the longest, and the strip is 2013.
      vertex_indices[0] = builder.createTriOp(
          spv::OpSelect, type_int,
          builder.createBinOp(spv::OpLogicalAnd, type_bool,
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  edge_lengths[0], edge_lengths[1]),
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  edge_lengths[0], edge_lengths[2])),
          const_int_0,
          builder.createTriOp(spv::OpSelect, type_int,
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  edge_lengths[1], edge_lengths[2]),
                              const_int_1, const_int_2));
      for (uint32_t i = 1; i < 3; ++i) {
        // vertex_indices[i] = (vertex_indices[0] + i) % 3
        spv::Id vertex_index_without_wrapping = builder.createBinOp(
            spv::OpIAdd, type_int, vertex_indices[0], builder.makeIntConstant(int32_t(i)));
        vertex_indices[i] = builder.createTriOp(
            spv::OpSelect, type_int,
            builder.createBinOp(spv::OpSLessThan, type_bool, vertex_index_without_wrapping,
                                const_int_3),
            vertex_index_without_wrapping,
            builder.createBinOp(spv::OpISub, type_int, vertex_index_without_wrapping, const_int_3));
      }

      // Initialize the point coordinates output for safety if this shader type
      // is used with has_point_coordinates for some reason.
      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (key.has_point_coordinates) {
        spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero = builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      // Emit the triangle in the strip that consists of the original vertices.
      for (uint32_t i = 0; i < 3; ++i) {
        spv::Id vertex_index = vertex_indices[i];
        // Interpolators.
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_index);
        for (uint32_t j = 0; j < interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(spv::StorageClassInput,
                                                           in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        // Point coordinates.
        if (key.has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero, out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_index);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
        spv::Id vertex_position = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            vertex_position,
            builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(vertex_index);
          id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
          spv::Id vertex_clip_distances = builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision);
          id_vector_temp.clear();
          id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(vertex_clip_distances,
                              builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex,
                                                        id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }

      // Construct the fourth vertex.
      // Interpolators.
      for (uint32_t i = 0; i < interpolator_count; ++i) {
        spv::Id in_interpolator = in_interpolators[i];
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_indices[0]);
        spv::Id vertex_interpolator_v0 = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_interpolator, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = vertex_indices[1];
        spv::Id vertex_interpolator_v01 = builder.createNoContractionBinOp(
            spv::OpFSub, type_float4,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_interpolator, id_vector_temp),
                spv::NoPrecision),
            vertex_interpolator_v0);
        id_vector_temp[0] = vertex_indices[2];
        spv::Id vertex_interpolator_v3 = builder.createNoContractionBinOp(
            spv::OpFAdd, type_float4, vertex_interpolator_v01,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_interpolator, id_vector_temp),
                spv::NoPrecision));
        builder.createStore(vertex_interpolator_v3, out_interpolators[i]);
      }
      // Point coordinates.
      if (key.has_point_coordinates) {
        builder.createStore(const_point_coordinates_zero, out_point_coordinates);
      }
      // Position.
      id_vector_temp.clear();
      id_vector_temp.push_back(vertex_indices[0]);
      id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
      spv::Id vertex_position_v0 = builder.createLoad(
          builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
          spv::NoPrecision);
      id_vector_temp[0] = vertex_indices[1];
      spv::Id vertex_position_v01 = builder.createNoContractionBinOp(
          spv::OpFSub, type_float4,
          builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision),
          vertex_position_v0);
      id_vector_temp[0] = vertex_indices[2];
      spv::Id vertex_position_v3 = builder.createNoContractionBinOp(
          spv::OpFAdd, type_float4, vertex_position_v01,
          builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision));
      id_vector_temp.clear();
      id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
      builder.createStore(
          vertex_position_v3,
          builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
      // Clip distances.
      for (uint32_t i = 0; i < clip_distance_count; ++i) {
        spv::Id const_int_i = builder.makeIntConstant(int32_t(i));
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_indices[0]);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        id_vector_temp.push_back(const_int_i);
        spv::Id vertex_clip_distance_v0 = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = vertex_indices[1];
        spv::Id vertex_clip_distance_v01 = builder.createNoContractionBinOp(
            spv::OpFSub, type_float,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision),
            vertex_clip_distance_v0);
        id_vector_temp[0] = vertex_indices[2];
        spv::Id vertex_clip_distance_v3 = builder.createNoContractionBinOp(
            spv::OpFAdd, type_float, vertex_clip_distance_v01,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision));
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
        id_vector_temp.push_back(const_int_i);
        builder.createStore(
            vertex_clip_distance_v3,
            builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
      }
      // Emit the vertex.
      builder.createNoResultOp(spv::OpEmitVertex);
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case PipelineGeometryShader::kQuadList: {
      // Initialize the point coordinates output for safety if this shader type
      // is used with has_point_coordinates for some reason.
      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (key.has_point_coordinates) {
        spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero = builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      // Build the triangle strip from the original quad vertices in the
      // 0, 1, 3, 2 order (like specified for GL_QUAD_STRIP).
      // TODO(Triang3l): Find the correct decomposition of quads into triangles
      // on the real hardware.
      for (uint32_t i = 0; i < 4; ++i) {
        spv::Id const_vertex_index = builder.makeIntConstant(int32_t(i ^ (i >> 1)));
        // Interpolators.
        id_vector_temp.clear();
        id_vector_temp.push_back(const_vertex_index);
        for (uint32_t j = 0; j < interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(spv::StorageClassInput,
                                                           in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        // Point coordinates.
        if (key.has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero, out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(const_vertex_index);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
        spv::Id vertex_position = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            vertex_position,
            builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(const_vertex_index);
          id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
          spv::Id vertex_clip_distances = builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision);
          id_vector_temp.clear();
          id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(vertex_clip_distances,
                              builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex,
                                                        id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    default:
      assert_unhandled_case(key.type);
  }

  // End the main function.
  builder.leaveFunction();

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the shader module, and store the handle even if creation fails not
  // to try to create it again later.
  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      command_processor_.GetVulkanDevice(), reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (shader_module == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "VulkanPipelineCache: Failed to create the primitive type geometry "
        "shader 0x{:08X}",
        key.key);
  }
  geometry_shaders_.emplace(key, shader_module);
  return shader_module;
}

bool VulkanPipelineCache::TryGetPipelineCreationArgumentsForDescription(
    const PipelineDescription& description,
    std::pair<const PipelineDescription, Pipeline>* pipeline,
    PipelineCreationArguments& creation_arguments, bool for_placeholder,
    bool allow_untranslated) {
  auto fail = [&](const char* reason) {
    REXGPU_ERROR(
        "VulkanPipelineCache: TryGetPipelineCreationArgumentsForDescription "
        "failed: {} (vs={:016X}, ps={:016X}, topo={}, geom={}, tess_mode={}, "
        "for_placeholder={}, render_pass_key=0x{:08X})",
        reason, description.vertex_shader_hash, description.pixel_shader_hash,
        uint32_t(description.primitive_topology), uint32_t(description.geometry_shader),
        uint32_t(description.tessellation_mode), uint32_t(for_placeholder),
        description.render_pass_key.key);
    return false;
  };

  if (!pipeline) {
    return fail("null_pipeline_storage");
  }
  if (!ArePipelineRequirementsMet(description)) {
    return fail("pipeline_requirements_not_met");
  }

  auto vertex_shader_it = shaders_.find(description.vertex_shader_hash);
  if (vertex_shader_it == shaders_.end()) {
    return fail("vertex_shader_not_found");
  }
  auto* vertex_shader = static_cast<VulkanShader::VulkanTranslation*>(
      vertex_shader_it->second->GetTranslation(description.vertex_shader_modification));
  // allow_untranslated: a shader not translated yet is translated on the creation thread
  // (TranslateShadersForCreation); one attempted before and invalid is still rejected.
  if (!vertex_shader ||
      (allow_untranslated ? (vertex_shader->is_translated() && !vertex_shader->is_valid())
                          : (!vertex_shader->is_translated() || !vertex_shader->is_valid()))) {
    return fail("vertex_shader_translation_missing_or_invalid");
  }
  SpirvShaderTranslator::Modification vertex_shader_modification(
      description.vertex_shader_modification);
  Shader::HostVertexShaderType host_vertex_shader_type =
      vertex_shader_modification.vertex.host_vertex_shader_type;
  bool tessellated = Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type);

  VulkanShader::VulkanTranslation* pixel_shader = nullptr;
  if (description.pixel_shader_hash && !for_placeholder) {
    auto pixel_shader_it = shaders_.find(description.pixel_shader_hash);
    if (pixel_shader_it == shaders_.end()) {
      return fail("pixel_shader_not_found");
    }
    pixel_shader = static_cast<VulkanShader::VulkanTranslation*>(
        pixel_shader_it->second->GetTranslation(description.pixel_shader_modification));
    if (!pixel_shader ||
        (allow_untranslated ? (pixel_shader->is_translated() && !pixel_shader->is_valid())
                            : (!pixel_shader->is_translated() || !pixel_shader->is_valid()))) {
      return fail("pixel_shader_translation_missing_or_invalid");
    }
  }
  SpirvShaderTranslator::Modification pixel_shader_modification(
      description.pixel_shader_modification);

  VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
  VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
  uint32_t tessellation_patch_control_points = 0;
  if (tessellated) {
    xenos::TessellationMode tessellation_mode = description.tessellation_mode;
    tessellation_patch_control_points =
        GetTessellationPatchControlPointCount(host_vertex_shader_type, tessellation_mode);
    if (!tessellation_patch_control_points) {
      return fail("tessellation_patch_control_points_zero");
    }
    tessellation_vertex_shader =
        GetTessellationVertexShader(tessellation_mode == xenos::TessellationMode::kAdaptive);
    if (tessellation_vertex_shader == VK_NULL_HANDLE) {
      return fail("tessellation_vertex_shader_unavailable");
    }
    tessellation_control_shader =
        GetTessellationControlShader(host_vertex_shader_type, tessellation_mode);
    if (tessellation_control_shader == VK_NULL_HANDLE) {
      return fail("tessellation_control_shader_unavailable");
    }
  }

  VkShaderModule geometry_shader = VK_NULL_HANDLE;
  GeometryShaderKey geometry_shader_key;
  if (GetGeometryShaderKey(description.geometry_shader, vertex_shader_modification,
                           pixel_shader_modification, geometry_shader_key)) {
    geometry_shader = GetGeometryShader(geometry_shader_key);
    if (geometry_shader == VK_NULL_HANDLE) {
      return fail("geometry_shader_unavailable");
    }
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && vulkan_device->properties().dynamicRendering;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (!use_dynamic_rendering) {
    render_pass =
        render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock
            ? render_target_cache_.GetFragmentShaderInterlockRenderPass()
            : render_target_cache_.GetHostRenderTargetsRenderPass(description.render_pass_key);
    if (render_pass == VK_NULL_HANDLE) {
      return fail("render_pass_unavailable");
    }
  }

  // The layout depends on the texture bindings known after translation: with a shader still
  // to be translated it is filled in by TranslateShadersForCreation on the creation thread.
  const bool layout_later =
      allow_untranslated &&
      (!vertex_shader->is_translated() ||
       (pixel_shader && !for_placeholder && !pixel_shader->is_translated()));
  const PipelineLayoutProvider* pipeline_layout = layout_later ? nullptr : command_processor_.GetPipelineLayout(
      (!for_placeholder && pixel_shader) ? static_cast<const VulkanShader&>(pixel_shader->shader())
                                               .GetTextureBindingsAfterTranslation()
                                               .size()
                                         : 0,
      (!for_placeholder && pixel_shader) ? static_cast<const VulkanShader&>(pixel_shader->shader())
                                               .GetSamplerBindingsAfterTranslation()
                                               .size()
                                         : 0,
      static_cast<const VulkanShader&>(vertex_shader->shader())
          .GetTextureBindingsAfterTranslation()
          .size(),
      static_cast<const VulkanShader&>(vertex_shader->shader())
          .GetSamplerBindingsAfterTranslation()
          .size());
  if (!pipeline_layout && !layout_later) {
    return fail("pipeline_layout_unavailable");
  }

  creation_arguments.pipeline = pipeline;
  creation_arguments.pipeline_layout = pipeline_layout;
  creation_arguments.vertex_shader = vertex_shader;
  creation_arguments.pixel_shader = for_placeholder ? nullptr : pixel_shader;
  creation_arguments.tessellation_vertex_shader = tessellation_vertex_shader;
  creation_arguments.tessellation_control_shader = tessellation_control_shader;
  creation_arguments.tessellation_patch_control_points = tessellation_patch_control_points;
  creation_arguments.geometry_shader = geometry_shader;
  creation_arguments.render_pass = render_pass;
  return true;
}

namespace {
uint64_t FragmentLibraryKeyHash(uint64_t pixel_shader_hash, uint64_t pixel_shader_variant,
                                const VkPipelineMultisampleStateCreateInfo& multisample,
                                uint32_t texture_sign_classes = 0) {
  struct {
    uint32_t kind = 3;
    uint32_t samples;
    uint32_t sample_mask;
    uint32_t sample_shading;
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_variant;
    // The pixel shader SPIR-V depends on translator switches (the driver cache is keyed by it):
    // part of the key, so a library list entry never claims a library of another translation.
    uint32_t translation_switches;
    uint32_t texture_sign_classes;
  } key;
  key.samples = uint32_t(multisample.rasterizationSamples);
  key.sample_mask = multisample.pSampleMask ? *multisample.pSampleMask : UINT32_MAX;
  key.sample_shading = multisample.sampleShadingEnable;
  key.pixel_shader_hash = pixel_shader_hash;
  key.pixel_shader_variant = pixel_shader_variant;
  key.translation_switches =
      uint32_t(REXCVAR_GET(spirv_texture_sign_branch)) |
      (uint32_t(REXCVAR_GET(vulkan_shader_xenos_multiply)) << 1) |
      (uint32_t(REXCVAR_GET(vulkan_texture_fetch_implicit_lod)) << 2) |
      (uint32_t(SpirvShaderTranslator::IsTextureSignSpecializationEnabled()) << 3) |
      (uint32_t(REXCVAR_GET(spirv_structured_forward_jumps)) << 4);
  key.texture_sign_classes = texture_sign_classes;
  return XXH3_64bits(&key, sizeof(key));
}

// Specialization constants of a pixel shader for its texture sign classes (see
// SpirvShaderTranslator::kTextureSignClassSpecIdBase); nullptr when all are runtime.
struct TextureSignSpecialization {
  VkSpecializationMapEntry entries[SpirvShaderTranslator::kTextureSignClassSlots];
  uint32_t data[SpirvShaderTranslator::kTextureSignClassSlots];
  VkSpecializationInfo info;
  // Vertex shaders: slots = SpirvShaderTranslator::kVertexTextureSignClassSlots.
  const VkSpecializationInfo* Build(uint32_t texture_sign_classes,
                                    uint32_t pixel_shader_used_texture_mask,
                                    uint32_t slots = SpirvShaderTranslator::kTextureSignClassSlots) {
    uint32_t count = 0;
    uint32_t remaining = texture_sign_classes ? pixel_shader_used_texture_mask : 0;
    for (uint32_t slot = 0; slot < slots && remaining; ++slot) {
      uint32_t fetch_constant_index = uint32_t(__builtin_ctz(remaining));
      remaining &= remaining - 1;
      uint32_t sign_class =
          (texture_sign_classes >> (SpirvShaderTranslator::kTextureSignClassBits * slot)) &
          ((UINT32_C(1) << SpirvShaderTranslator::kTextureSignClassBits) - 1);
      if (!sign_class) {
        continue;
      }
      entries[count].constantID =
          SpirvShaderTranslator::kTextureSignClassSpecIdBase + fetch_constant_index;
      entries[count].offset = uint32_t(sizeof(uint32_t) * count);
      entries[count].size = sizeof(uint32_t);
      data[count] = sign_class |
                    ((texture_sign_classes & SpirvShaderTranslator::kTextureSignClassesExpAdjustZero)
                         ? SpirvShaderTranslator::kTextureSignClassExpAdjustZeroBit
                         : 0);
      ++count;
    }
    if (!count) {
      return nullptr;
    }
    info.mapEntryCount = count;
    info.pMapEntries = entries;
    info.dataSize = sizeof(uint32_t) * count;
    info.pData = data;
    return &info;
  }
};

uint32_t GetPixelShaderUsedTextureMask(const VulkanShader::VulkanTranslation* translation) {
  return translation
             ? static_cast<const VulkanShader&>(translation->shader())
                   .GetUsedTextureMaskAfterTranslation()
             : 0;
}
}  // namespace

bool VulkanPipelineCache::IsLibraryAvailable(uint64_t key_hash) {
  std::lock_guard<std::mutex> lock(pipeline_libraries_mutex_);
  return pipeline_libraries_.count(key_hash) || known_library_keys_.count(key_hash);
}

void VulkanPipelineCache::LoadLibraryKeys(const std::filesystem::path& path) {
  // 'XELK', vendor, device, pipelineCacheUUID[16], then u64 key hashes (appended as created).
  const ui::vulkan::VulkanDevice::Properties& properties =
      command_processor_.GetVulkanDevice()->properties();
  uint8_t header[28];
  // The translator's output version is folded into the magic: a list from older SPIR-V is dropped.
  uint32_t magic = 0x4B4C4558 ^ (SpirvShaderTranslator::kOutputVersion << 24);
  std::memcpy(header, &magic, 4);
  std::memcpy(header + 4, &properties.vendorID, 4);
  std::memcpy(header + 8, &properties.deviceID, 4);
  std::memcpy(header + 12, properties.pipelineCacheUUID, 16);
  std::vector<uint8_t> data;
  if (FILE* file = rex::filesystem::OpenFile(path, "rb")) {
    rex::filesystem::Seek(file, 0, SEEK_END);
    int64_t size = rex::filesystem::Tell(file);
    rex::filesystem::Seek(file, 0, SEEK_SET);
    if (size >= int64_t(sizeof(header)) && size < (int64_t(64) << 20)) {
      data.resize(size_t(size));
      if (fread(data.data(), 1, data.size(), file) != data.size()) {
        data.clear();
      }
    }
    fclose(file);
  }
  bool valid = data.size() >= sizeof(header) && !std::memcmp(data.data(), header, sizeof(header));
  if (valid) {
    for (size_t offset = sizeof(header); offset + 8 <= data.size(); offset += 8) {
      uint64_t key_hash;
      std::memcpy(&key_hash, data.data() + offset, 8);
      known_library_keys_.insert(key_hash);
    }
    library_keys_file_ = OpenStorageFileForAppend(path);
  } else {
    library_keys_file_ = rex::filesystem::OpenFile(path, "wb");
    if (library_keys_file_) {
      fwrite(header, 1, sizeof(header), library_keys_file_);
      fflush(library_keys_file_);
    }
  }
  REXGPU_INFO("VulkanPipelineCache: {} pipeline libraries known in the driver cache{}",
              known_library_keys_.size(), valid ? "" : " (new list for this driver build)");
}

VkPipeline VulkanPipelineCache::GetOrCreatePipelineLibrary(
    const void* key, size_t key_size, VkGraphicsPipelineCreateInfo& create_info,
    VkGraphicsPipelineLibraryFlagsEXT library_flags, bool only_if_cached) {
  return GetOrCreatePipelineLibraryByHash(XXH3_64bits(key, key_size), create_info, library_flags,
                                          only_if_cached);
}

VkPipeline VulkanPipelineCache::GetOrCreatePipelineLibraryByHash(
    uint64_t key_hash, VkGraphicsPipelineCreateInfo& create_info,
    VkGraphicsPipelineLibraryFlagsEXT library_flags, bool only_if_cached) {
  {
    std::lock_guard<std::mutex> lock(pipeline_libraries_mutex_);
    auto it = pipeline_libraries_.find(key_hash);
    if (it != pipeline_libraries_.end()) {
      return it->second;
    }
    if (only_if_cached && !known_library_keys_.count(key_hash)) {
      return VK_NULL_HANDLE;
    }
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  VkGraphicsPipelineLibraryCreateInfoEXT library_info = {
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
  library_info.pNext = create_info.pNext;
  library_info.flags = library_flags;
  create_info.pNext = &library_info;
  create_info.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
  auto start = std::chrono::steady_clock::now();
  VkPipeline library = VK_NULL_HANDLE;
  VkResult result = vulkan_device->functions().vkCreateGraphicsPipelines(
      vulkan_device->device(), vulkan_device->pipeline_cache(), 1, &create_info, nullptr,
      &library);
  create_info.pNext = library_info.pNext;
  if (result != VK_SUCCESS) {
    REXGPU_ERROR("VulkanPipelineCache: failed to create a pipeline library (flags 0x{:X}, {})",
                 uint32_t(library_flags), int32_t(result));
    return VK_NULL_HANDLE;
  }
  MarkDriverPipelineCacheUsed();
  counter_libraries_created_.fetch_add(1, std::memory_order_relaxed);
  counter_libraries_created_us_.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count()),
      std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(pipeline_libraries_mutex_);
  auto emplaced = pipeline_libraries_.emplace(key_hash, library);
  if (!emplaced.second) {
    // Created concurrently by another thread - keep the first one.
    vulkan_device->functions().vkDestroyPipeline(vulkan_device->device(), library, nullptr);
    return emplaced.first->second;
  }
  // Recorded in the key list only once a saved driver cache contains it (SaveDriverPipelineCache),
  // so the list never claims a library the cache on disk can't produce.
  if (known_library_keys_.insert(key_hash).second) {
    unsaved_library_keys_.push_back(key_hash);
  }
  return library;
}

VkPipeline VulkanPipelineCache::GetOrCreateFragmentLibrary(
    uint64_t key_hash, VkShaderModule fragment_module, VkPipelineLayout layout,
    const VkPipelineMultisampleStateCreateInfo& multisample,
    const VkSpecializationInfo* specialization) {
  VkPipelineRenderingCreateInfo view_mask_rendering = {
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stage.module = fragment_module;
  stage.pName = "main";
  stage.pSpecializationInfo = specialization;
  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  static const VkDynamicState kDynamicStates[] = {
      VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,  VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,   VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
      VK_DYNAMIC_STATE_STENCIL_OP,         VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
      // Last: only with dynamic_fragment_shading_rate_ (Eco mode).
      VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR,
  };
  VkPipelineDynamicStateCreateInfo dynamic_state = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic_state.dynamicStateCount =
      uint32_t(rex::countof(kDynamicStates)) - (dynamic_fragment_shading_rate_ ? 0 : 1);
  dynamic_state.pDynamicStates = kDynamicStates;
  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &view_mask_rendering;
  info.stageCount = fragment_module != VK_NULL_HANDLE ? 1 : 0;
  info.pStages = fragment_module != VK_NULL_HANDLE ? &stage : nullptr;
  info.pDepthStencilState = &depth_stencil_state;
  info.pMultisampleState = &multisample;
  info.pDynamicState = &dynamic_state;
  info.layout = layout;
  return GetOrCreatePipelineLibraryByHash(key_hash, info,
                                          VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
                                          false);
}

uint64_t VulkanPipelineCache::PreRasterizationLibraryKeyHash(uint64_t vertex_shader_hash,
                                                             uint64_t vertex_shader_modification,
                                                             uint32_t vertex_texture_sign_classes,
                                                             uint32_t geometry_shader_key) {
  // Stable across runs: the shader's ucode hash + modification determine its translation (and its
  // texture set), the geometry shader key the expansion shader.
  struct {
    uint32_t kind = 2;
    uint32_t geometry_shader_key = 0;
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // The vertex shader SPIR-V depends on translator switches (the driver cache is keyed by it):
    // part of the key, so the library list never claims a library of another translation. Without
    // them, after the structured-jump switch the list claimed every vertex library, draws linked
    // "cached" libraries on the command thread and the driver compiled them there (130-150 ms
    // stalls, 2026-11-11).
    uint32_t translation_switches;
    uint32_t reserved = 0;
  } key;
  key.vertex_shader_hash = vertex_shader_hash;
  key.vertex_shader_modification = vertex_shader_modification;
  // Vertex texture sign classes above the kind (0 = runtime).
  key.kind |= vertex_texture_sign_classes << 8;
  key.geometry_shader_key = geometry_shader_key;
  key.translation_switches =
      uint32_t(REXCVAR_GET(spirv_texture_sign_branch)) |
      (uint32_t(REXCVAR_GET(vulkan_shader_xenos_multiply)) << 1) |
      (uint32_t(REXCVAR_GET(vulkan_shader_xenos_multiply_vertex)) << 2) |
      (uint32_t(REXCVAR_GET(vulkan_shared_memory_texel_buffer)) << 3) |
      (uint32_t(REXCVAR_GET(spirv_structured_forward_jumps)) << 4);
  return XXH3_64bits(&key, sizeof(key));
}

VkPipeline VulkanPipelineCache::GetOrCreatePreRasterizationLibrary(
    const VulkanShader::VulkanTranslation* vertex_shader, uint64_t vertex_shader_modification,
    uint32_t vertex_texture_sign_classes, uint32_t geometry_shader_key,
    VkShaderModule geometry_shader, VkPipelineLayout layout, bool only_if_cached) {
  // Shared by the draws (EnsurePipelineCreatedFromLibraries) and the boot precompile, so both
  // produce the same key and the same library.
  uint64_t key_hash = PreRasterizationLibraryKeyHash(vertex_shader->shader().ucode_data_hash(),
                                                     vertex_shader_modification,
                                                     vertex_texture_sign_classes,
                                                     geometry_shader_key);
  TextureSignSpecialization vertex_sign_specialization;
  VkPipelineShaderStageCreateInfo stages[2] = {};
  uint32_t stage_count = 0;
  stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[stage_count].module = vertex_shader->shader_module();
  stages[stage_count].pName = "main";
  stages[stage_count].pSpecializationInfo = vertex_sign_specialization.Build(
      vertex_texture_sign_classes, GetPixelShaderUsedTextureMask(vertex_shader),
      SpirvShaderTranslator::kVertexTextureSignClassSlots);
  ++stage_count;
  if (geometry_shader != VK_NULL_HANDLE) {
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    stages[stage_count].module = geometry_shader;
    stages[stage_count].pName = "main";
    ++stage_count;
  }
  VkPipelineRenderingCreateInfo view_mask_rendering = {
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  view_mask_rendering.viewMask = 0;
  VkPipelineViewportStateCreateInfo viewport_state = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rasterization_state = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization_state.lineWidth = 1.0f;
  static const VkDynamicState kDynamicStates[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
      VK_DYNAMIC_STATE_CULL_MODE,
      VK_DYNAMIC_STATE_FRONT_FACE,
      VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
      VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
      VK_DYNAMIC_STATE_POLYGON_MODE_EXT,
      VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT,
      // Last: only with dynamic_fragment_shading_rate_ (Eco mode).
      VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR,
  };
  VkPipelineDynamicStateCreateInfo dynamic_state = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic_state.dynamicStateCount =
      uint32_t(rex::countof(kDynamicStates)) - (dynamic_fragment_shading_rate_ ? 0 : 1);
  dynamic_state.pDynamicStates = kDynamicStates;
  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &view_mask_rendering;
  info.stageCount = stage_count;
  info.pStages = stages;
  info.pViewportState = &viewport_state;
  info.pRasterizationState = &rasterization_state;
  info.pDynamicState = &dynamic_state;
  info.layout = layout;
  return GetOrCreatePipelineLibraryByHash(
      key_hash, info, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
      only_if_cached);
}

bool VulkanPipelineCache::EnsurePipelineCreatedFromLibraries(
    const PipelineCreationArguments& creation_arguments, bool only_if_cached) {
  const PipelineDescription& description = creation_arguments.pipeline->first;
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  if (description.primitive_topology == PipelinePrimitiveTopology::kPatchList) {
    static bool tessellation_logged = false;
    if (!tessellation_logged) {
      tessellation_logged = true;
      REXGPU_WARN("VulkanPipelineCache: tessellated draws are not supported with pipeline "
                  "libraries (vulkan_pipeline_libraries); skipping them");
    }
    return false;
  }
  assert_true(creation_arguments.vertex_shader->is_translated());
  if (!creation_arguments.vertex_shader->is_valid()) {
    return false;
  }
  const PipelineLayoutProvider* layout_provider = creation_arguments.pipeline_layout;
  VkPipelineLayout pre_rasterization_layout = layout_provider->GetPreRasterizationLibraryLayout();
  VkPipelineLayout fragment_layout = layout_provider->GetFragmentLibraryLayout();
  if (pre_rasterization_layout == VK_NULL_HANDLE || fragment_layout == VK_NULL_HANDLE) {
    return false;
  }
  auto link_start = std::chrono::steady_clock::now();

  // Shared pieces.
  VkPipelineRenderingCreateInfo view_mask_rendering = {
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  view_mask_rendering.viewMask = 0;

  // Multisample state (identical in the fragment shader and output libraries).
  const auto& rp_key = description.render_pass_key;
  bool has_attachments = rp_key.depth_and_color_used != 0;
  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  bool sample_mask_used = false;
  if (description.rasterizer_discard || !has_attachments) {
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  } else if (rp_key.msaa_samples == xenos::MsaaSamples::k2X &&
             !render_target_cache_.IsMsaa2xSupported(has_attachments)) {
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    sample_mask = 0b1001;
    multisample_state.pSampleMask = &sample_mask;
    sample_mask_used = true;
  } else {
    multisample_state.rasterizationSamples =
        VkSampleCountFlagBits(uint32_t(1) << uint32_t(rp_key.msaa_samples));
  }
  if (description.sample_rate_shading &&
      multisample_state.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT) {
    multisample_state.sampleShadingEnable = VK_TRUE;
    multisample_state.minSampleShading = 1.0f;
  }

  // 1. Vertex input interface: topology and primitive restart (no vertex buffers - the shaders
  // fetch from the shared memory).
  VkPipeline vertex_input_library;
  {
    VkPrimitiveTopology topology;
    switch (description.primitive_topology) {
      case PipelinePrimitiveTopology::kPointList:
        topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        break;
      case PipelinePrimitiveTopology::kLineList:
        topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        break;
      case PipelinePrimitiveTopology::kLineStrip:
        topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        break;
      case PipelinePrimitiveTopology::kTriangleList:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        break;
      case PipelinePrimitiveTopology::kTriangleStrip:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        break;
      case PipelinePrimitiveTopology::kLineListWithAdjacency:
        topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
        break;
      default:
        return false;
    }
    struct {
      uint32_t kind = 1;
      uint32_t topology;
      uint32_t primitive_restart;
    } key;
    key.topology = uint32_t(topology);
    key.primitive_restart = description.primitive_restart;
    VkPipelineVertexInputStateCreateInfo vertex_input_state = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly_state.topology = topology;
    input_assembly_state.primitiveRestartEnable =
        description.primitive_restart ? VK_TRUE : VK_FALSE;
    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pVertexInputState = &vertex_input_state;
    info.pInputAssemblyState = &input_assembly_state;
    vertex_input_library = GetOrCreatePipelineLibrary(
        &key, sizeof(key), info, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
        only_if_cached);
    if (vertex_input_library == VK_NULL_HANDLE) {
      return false;
    }
  }

  // 2. Pre-rasterization shaders: the vertex shader (and the primitive expansion geometry shader),
  // rasterization state dynamic.
  VkPipeline pre_rasterization_library;
  {
    uint32_t geometry_shader_key_value = 0;
    GeometryShaderKey geometry_shader_key;
    if (GetGeometryShaderKey(
            description.geometry_shader,
            SpirvShaderTranslator::Modification(description.vertex_shader_modification),
            SpirvShaderTranslator::Modification(description.pixel_shader_modification),
            geometry_shader_key)) {
      geometry_shader_key_value = geometry_shader_key.key;
    }
    pre_rasterization_library = GetOrCreatePreRasterizationLibrary(
        creation_arguments.vertex_shader, description.vertex_shader_modification,
        0, geometry_shader_key_value,
        creation_arguments.geometry_shader, pre_rasterization_layout, only_if_cached);
    if (pre_rasterization_library == VK_NULL_HANDLE) {
      return false;
    }
  }

  // 3. Fragment shader: the pixel shader (or a depth-only helper), depth / stencil dynamic.
  VkPipeline fragment_library;
  {
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    if (creation_arguments.pixel_shader) {
      assert_true(creation_arguments.pixel_shader->is_translated());
      if (!creation_arguments.pixel_shader->is_valid()) {
        return false;
      }
      fragment_module = creation_arguments.pixel_shader->shader_module();
    } else if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
               (rp_key.depth_and_color_used & 1) &&
               (description.depth_compare_op != xenos::CompareFunction::kAlways ||
                description.depth_write_enable) &&
               rp_key.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      fragment_module = render_target_cache_.depth_float24_round()
                            ? depth_float24_round_fragment_shader_
                            : depth_float24_truncate_fragment_shader_;
      if (fragment_module == VK_NULL_HANDLE) {
        return false;
      }
    }
    uint64_t fragment_key_hash = FragmentLibraryKeyHash(
        creation_arguments.pixel_shader
            ? creation_arguments.pixel_shader->shader().ucode_data_hash()
            : 0,
        creation_arguments.pixel_shader ? description.pixel_shader_modification
        : fragment_module == VK_NULL_HANDLE ? 0
        : fragment_module == depth_float24_round_fragment_shader_ ? 1
                                                                 : 2,
        multisample_state,
        creation_arguments.pixel_shader ? description.TextureSignClassesWord() : 0);
    if (only_if_cached && !IsLibraryAvailable(fragment_key_hash)) {
      return false;
    }
    TextureSignSpecialization sign_specialization;
    const VkSpecializationInfo* fragment_specialization =
        creation_arguments.pixel_shader
            ? sign_specialization.Build(description.TextureSignClassesWord(),
                                        GetPixelShaderUsedTextureMask(creation_arguments.pixel_shader))
            : nullptr;
    fragment_library =
        GetOrCreateFragmentLibrary(fragment_key_hash, fragment_module, fragment_layout,
                                   multisample_state, fragment_specialization);
    if (fragment_library == VK_NULL_HANDLE) {
      return false;
    }
  }

  // 4. Fragment output interface: attachment formats, blending dynamic.
  VkPipeline fragment_output_library;
  uint32_t color_rts_used = rp_key.depth_and_color_used >> 1;
  uint32_t color_attachment_count = 32 - rex::lzcnt(color_rts_used);
  {
    VkFormat color_formats[xenos::kMaxColorRenderTargets] = {};
    xenos::ColorRenderTargetFormat guest_color_formats[] = {
        rp_key.color_0_view_format, rp_key.color_1_view_format, rp_key.color_2_view_format,
        rp_key.color_3_view_format};
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (color_rts_used & (1 << i)) {
        color_formats[i] = render_target_cache_.GetColorVulkanFormat(guest_color_formats[i]);
      }
    }
    VkFormat depth_format = (rp_key.depth_and_color_used & 0b1)
                                ? render_target_cache_.GetDepthVulkanFormat(rp_key.depth_format)
                                : VK_FORMAT_UNDEFINED;
    struct {
      uint32_t kind = 4;
      uint32_t samples;
      uint32_t sample_mask;
      uint32_t sample_shading;
      uint32_t color_attachment_count;
      uint32_t depth_format;
      uint32_t color_formats[xenos::kMaxColorRenderTargets];
    } key;
    key.samples = uint32_t(multisample_state.rasterizationSamples);
    key.sample_mask = sample_mask_used ? sample_mask : UINT32_MAX;
    key.sample_shading = multisample_state.sampleShadingEnable;
    key.color_attachment_count = color_attachment_count;
    key.depth_format = uint32_t(depth_format);
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      key.color_formats[i] = uint32_t(color_formats[i]);
    }
    VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = color_attachment_count;
    rendering.pColorAttachmentFormats = color_attachment_count ? color_formats : nullptr;
    rendering.depthAttachmentFormat = depth_format;
    rendering.stencilAttachmentFormat = depth_format;
    VkPipelineColorBlendAttachmentState attachments[xenos::kMaxColorRenderTargets] = {};
    VkPipelineColorBlendStateCreateInfo color_blend_state = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend_state.attachmentCount = color_attachment_count;
    color_blend_state.pAttachments = attachments;
    static const VkDynamicState kDynamicStates[] = {
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
        VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
        VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,
    };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_state.dynamicStateCount = uint32_t(rex::countof(kDynamicStates));
    dynamic_state.pDynamicStates = kDynamicStates;
    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.pColorBlendState = &color_blend_state;
    info.pMultisampleState = &multisample_state;
    info.pDynamicState = &dynamic_state;
    fragment_output_library = GetOrCreatePipelineLibrary(
        &key, sizeof(key), info, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
        only_if_cached);
    if (fragment_output_library == VK_NULL_HANDLE) {
      return false;
    }
  }

  // Dynamic state values of this pipeline.
  PipelineDynamicState dynamic;
  dynamic.cull_mode = VK_CULL_MODE_NONE;
  if (description.cull_front) {
    dynamic.cull_mode |= VK_CULL_MODE_FRONT_BIT;
  }
  if (description.cull_back) {
    dynamic.cull_mode |= VK_CULL_MODE_BACK_BIT;
  }
  dynamic.front_face =
      description.front_face_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  if (description.depth_write_enable ||
      description.depth_compare_op != xenos::CompareFunction::kAlways) {
    dynamic.depth_test_enable = VK_TRUE;
    dynamic.depth_write_enable = description.depth_write_enable ? VK_TRUE : VK_FALSE;
    dynamic.depth_compare_op =
        VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(description.depth_compare_op));
  }
  if (description.stencil_test_enable) {
    dynamic.stencil_test_enable = VK_TRUE;
    dynamic.stencil_front_ops[0] =
        VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_front_fail_op));
    dynamic.stencil_front_ops[1] =
        VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_front_pass_op));
    dynamic.stencil_front_ops[2] = VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                                               uint32_t(description.stencil_front_depth_fail_op));
    dynamic.stencil_front_compare_op = VkCompareOp(
        uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(description.stencil_front_compare_op));
    dynamic.stencil_back_ops[0] =
        VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_back_fail_op));
    dynamic.stencil_back_ops[1] =
        VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_back_pass_op));
    dynamic.stencil_back_ops[2] = VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                                              uint32_t(description.stencil_back_depth_fail_op));
    dynamic.stencil_back_compare_op = VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                                                  uint32_t(description.stencil_back_compare_op));
  }
  dynamic.depth_bias_enable = (rp_key.depth_and_color_used & 0b1) ? VK_TRUE : VK_FALSE;
  dynamic.rasterizer_discard_enable = description.rasterizer_discard ? VK_TRUE : VK_FALSE;
  switch (description.polygon_mode) {
    case PipelinePolygonMode::kLine:
      dynamic.polygon_mode = VK_POLYGON_MODE_LINE;
      break;
    case PipelinePolygonMode::kPoint:
      dynamic.polygon_mode = VK_POLYGON_MODE_POINT;
      break;
    default:
      dynamic.polygon_mode = VK_POLYGON_MODE_FILL;
      break;
  }
  dynamic.depth_clamp_enable = description.depth_clamp_enable ? VK_TRUE : VK_FALSE;
  dynamic.color_attachment_count = color_attachment_count;
  {
    static const VkBlendFactor kBlendFactorMap[] = {
        VK_BLEND_FACTOR_ZERO,           VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR,      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_DST_COLOR,      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
    };
    static const VkBlendOp kBlendOpMap[] = {
        VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
        VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
    for (uint32_t i = 0; i < color_attachment_count; ++i) {
      VkColorBlendEquationEXT& equation = dynamic.color_blend_equation[i];
      equation.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      equation.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
      equation.colorBlendOp = VK_BLEND_OP_ADD;
      equation.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      equation.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      equation.alphaBlendOp = VK_BLEND_OP_ADD;
      if (!(color_rts_used & (1 << i))) {
        continue;
      }
      const PipelineRenderTarget& color_rt = description.render_targets[i];
      if (color_rt.src_color_blend_factor != PipelineBlendFactor::kOne ||
          color_rt.dst_color_blend_factor != PipelineBlendFactor::kZero ||
          color_rt.color_blend_op != xenos::BlendOp::kAdd ||
          color_rt.src_alpha_blend_factor != PipelineBlendFactor::kOne ||
          color_rt.dst_alpha_blend_factor != PipelineBlendFactor::kZero ||
          color_rt.alpha_blend_op != xenos::BlendOp::kAdd) {
        dynamic.color_blend_enable[i] = VK_TRUE;
        equation.srcColorBlendFactor = kBlendFactorMap[uint32_t(color_rt.src_color_blend_factor)];
        equation.dstColorBlendFactor = kBlendFactorMap[uint32_t(color_rt.dst_color_blend_factor)];
        equation.colorBlendOp = kBlendOpMap[uint32_t(color_rt.color_blend_op)];
        equation.srcAlphaBlendFactor = kBlendFactorMap[uint32_t(color_rt.src_alpha_blend_factor)];
        equation.dstAlphaBlendFactor = kBlendFactorMap[uint32_t(color_rt.dst_alpha_blend_factor)];
        equation.alphaBlendOp = kBlendOpMap[uint32_t(color_rt.alpha_blend_op)];
      }
      dynamic.color_write_mask[i] = VkColorComponentFlags(color_rt.color_write_mask);
    }
  }

  // Fast link.
  VkPipeline libraries[] = {vertex_input_library, pre_rasterization_library, fragment_library,
                            fragment_output_library};
  VkPipelineLibraryCreateInfoKHR library_info = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
  library_info.libraryCount = uint32_t(rex::countof(libraries));
  library_info.pLibraries = libraries;
  VkGraphicsPipelineCreateInfo link_info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  link_info.pNext = &library_info;
  link_info.layout = layout_provider->GetPipelineLayout();
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (dfn.vkCreateGraphicsPipelines(device, vulkan_device->pipeline_cache(), 1, &link_info,
                                    nullptr, &pipeline) != VK_SUCCESS) {
    REXGPU_ERROR("VulkanPipelineCache: failed to link a pipeline from libraries (VS {:016X})",
                 creation_arguments.vertex_shader->shader().ucode_data_hash());
    return false;
  }
  counter_pipelines_linked_.fetch_add(1, std::memory_order_relaxed);
  counter_pipelines_linked_us_.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - link_start)
                   .count()),
      std::memory_order_relaxed);

  Pipeline& pipeline_entry = creation_arguments.pipeline->second;
  pipeline_entry.dynamic_state = dynamic;
  pipeline_entry.uses_dynamic_state.store(true, std::memory_order_release);
  pipeline_entry.pipeline_layout.store(creation_arguments.pipeline_layout,
                                       std::memory_order_release);
  pipeline_entry.pipeline.store(pipeline, std::memory_order_release);
  pipeline_entry.is_placeholder.store(false, std::memory_order_release);
  return true;
}

bool VulkanPipelineCache::EnsurePipelineCreated(const PipelineCreationArguments& creation_arguments,
                                                VkShaderModule fragment_shader_override) {
  VkPipeline existing_pipeline =
      creation_arguments.pipeline->second.pipeline.load(std::memory_order_acquire);
  bool is_placeholder =
      creation_arguments.pipeline->second.is_placeholder.load(std::memory_order_acquire);
  bool creating_placeholder = fragment_shader_override != VK_NULL_HANDLE;
  if (existing_pipeline != VK_NULL_HANDLE) {
    if (!is_placeholder || creating_placeholder) {
      return true;
    }
  }

  // This function preferably should validate the description to prevent
  // unsupported behavior that may be dangerous/crashing because pipelines can
  // be created from the disk storage.

  if (creation_arguments.pixel_shader) {
    REXGPU_INFO("Creating graphics pipeline state with VS {:016X}, PS {:016X}",
                creation_arguments.vertex_shader->shader().ucode_data_hash(),
                creation_arguments.pixel_shader->shader().ucode_data_hash());
  } else {
    REXGPU_INFO("Creating graphics pipeline state with VS {:016X}",
                creation_arguments.vertex_shader->shader().ucode_data_hash());
  }

  const PipelineDescription& description = creation_arguments.pipeline->first;
  if (!ArePipelineRequirementsMet(description)) {
    assert_always(
        "When creating a new pipeline, the description must not require "
        "unsupported features, and when loading the pipeline storage, "
        "pipelines with unsupported features must be filtered out");
    return false;
  }

  if (uses_pipeline_libraries_ && fragment_shader_override == VK_NULL_HANDLE) {
    return EnsurePipelineCreatedFromLibraries(creation_arguments);
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  bool tessellated = description.primitive_topology == PipelinePrimitiveTopology::kPatchList;

  std::array<VkPipelineShaderStageCreateInfo, 5> shader_stages;
  uint32_t shader_stage_count = 0;
  TextureSignSpecialization vertex_sign_specialization;

  // Vertex or tessellation evaluation shader (plus helper stages for tessellation).
  assert_true(creation_arguments.vertex_shader->is_translated());
  if (!creation_arguments.vertex_shader->is_valid()) {
    return false;
  }
  if (tessellated) {
    if (creation_arguments.tessellation_vertex_shader == VK_NULL_HANDLE ||
        creation_arguments.tessellation_control_shader == VK_NULL_HANDLE ||
        !creation_arguments.tessellation_patch_control_points) {
      return false;
    }

    VkPipelineShaderStageCreateInfo& shader_stage_vertex = shader_stages[shader_stage_count++];
    shader_stage_vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_vertex.pNext = nullptr;
    shader_stage_vertex.flags = 0;
    shader_stage_vertex.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stage_vertex.module = creation_arguments.tessellation_vertex_shader;
    shader_stage_vertex.pName = "main";
    shader_stage_vertex.pSpecializationInfo = nullptr;

    VkPipelineShaderStageCreateInfo& shader_stage_tess_control =
        shader_stages[shader_stage_count++];
    shader_stage_tess_control.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tess_control.pNext = nullptr;
    shader_stage_tess_control.flags = 0;
    shader_stage_tess_control.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    shader_stage_tess_control.module = creation_arguments.tessellation_control_shader;
    shader_stage_tess_control.pName = "main";
    shader_stage_tess_control.pSpecializationInfo = nullptr;

    VkPipelineShaderStageCreateInfo& shader_stage_tess_eval = shader_stages[shader_stage_count++];
    shader_stage_tess_eval.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tess_eval.pNext = nullptr;
    shader_stage_tess_eval.flags = 0;
    shader_stage_tess_eval.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    shader_stage_tess_eval.module = creation_arguments.vertex_shader->shader_module();
    assert_true(shader_stage_tess_eval.module != VK_NULL_HANDLE);
    shader_stage_tess_eval.pName = "main";
    shader_stage_tess_eval.pSpecializationInfo = nullptr;
  } else {
    VkPipelineShaderStageCreateInfo& shader_stage_vertex = shader_stages[shader_stage_count++];
    shader_stage_vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_vertex.pNext = nullptr;
    shader_stage_vertex.flags = 0;
    shader_stage_vertex.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stage_vertex.module = creation_arguments.vertex_shader->shader_module();
    assert_true(shader_stage_vertex.module != VK_NULL_HANDLE);
    shader_stage_vertex.pName = "main";
    shader_stage_vertex.pSpecializationInfo = vertex_sign_specialization.Build(
        0, GetPixelShaderUsedTextureMask(creation_arguments.vertex_shader),
        SpirvShaderTranslator::kVertexTextureSignClassSlots);
  }
  // Geometry shader.
  if (creation_arguments.geometry_shader != VK_NULL_HANDLE) {
    if (tessellated) {
      return false;
    }
    VkPipelineShaderStageCreateInfo& shader_stage_geometry = shader_stages[shader_stage_count++];
    shader_stage_geometry.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_geometry.pNext = nullptr;
    shader_stage_geometry.flags = 0;
    shader_stage_geometry.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    shader_stage_geometry.module = creation_arguments.geometry_shader;
    shader_stage_geometry.pName = "main";
    shader_stage_geometry.pSpecializationInfo = nullptr;
  }
  // Fragment shader.
  // Outlives the vkCreateGraphicsPipelines call below.
  TextureSignSpecialization fragment_sign_specialization;
  VkPipelineShaderStageCreateInfo& shader_stage_fragment = shader_stages[shader_stage_count++];
  shader_stage_fragment.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stage_fragment.pNext = nullptr;
  shader_stage_fragment.flags = 0;
  shader_stage_fragment.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shader_stage_fragment.module = VK_NULL_HANDLE;
  shader_stage_fragment.pName = "main";
  shader_stage_fragment.pSpecializationInfo = nullptr;
  if (fragment_shader_override != VK_NULL_HANDLE) {
    shader_stage_fragment.module = fragment_shader_override;
  } else if (creation_arguments.pixel_shader) {
    assert_true(creation_arguments.pixel_shader->is_translated());
    if (!creation_arguments.pixel_shader->is_valid()) {
      return false;
    }
    shader_stage_fragment.module = creation_arguments.pixel_shader->shader_module();
    assert_true(shader_stage_fragment.module != VK_NULL_HANDLE);
    shader_stage_fragment.pSpecializationInfo = fragment_sign_specialization.Build(
        description.TextureSignClassesWord(),
        GetPixelShaderUsedTextureMask(creation_arguments.pixel_shader));
  } else {
    if (edram_fragment_shader_interlock) {
      shader_stage_fragment.module = depth_only_fragment_shader_;
    } else if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
               (description.render_pass_key.depth_and_color_used & 1) &&
               (description.depth_compare_op != xenos::CompareFunction::kAlways ||
                description.depth_write_enable) &&
               description.render_pass_key.depth_format ==
                   xenos::DepthRenderTargetFormat::kD24FS8) {
      shader_stage_fragment.module = render_target_cache_.depth_float24_round()
                                         ? depth_float24_round_fragment_shader_
                                         : depth_float24_truncate_fragment_shader_;
      if (shader_stage_fragment.module == VK_NULL_HANDLE) {
        return false;
      }
    }
  }
  if (shader_stage_fragment.module == VK_NULL_HANDLE) {
    --shader_stage_count;
  }

  VkPipelineVertexInputStateCreateInfo vertex_input_state = {};
  vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
  input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.pNext = nullptr;
  input_assembly_state.flags = 0;
  switch (description.primitive_topology) {
    case PipelinePrimitiveTopology::kPointList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kLineList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kLineStrip:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kTriangleStrip:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleFan:
      // Keep parity with D3D12 by requiring triangle fan to list conversion in
      // PrimitiveProcessor rather than emitting native fan pipelines.
      assert_always();
      return false;
    case PipelinePrimitiveTopology::kLineListWithAdjacency:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kPatchList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    default:
      assert_unhandled_case(description.primitive_topology);
      return false;
  }
  input_assembly_state.primitiveRestartEnable = description.primitive_restart ? VK_TRUE : VK_FALSE;

  VkPipelineViewportStateCreateInfo viewport_state;
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.flags = 0;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = nullptr;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.rasterizerDiscardEnable = description.rasterizer_discard ? VK_TRUE : VK_FALSE;
  rasterization_state.depthClampEnable = description.depth_clamp_enable ? VK_TRUE : VK_FALSE;
  switch (description.polygon_mode) {
    case PipelinePolygonMode::kFill:
      rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
      break;
    case PipelinePolygonMode::kLine:
      rasterization_state.polygonMode = VK_POLYGON_MODE_LINE;
      break;
    case PipelinePolygonMode::kPoint:
      rasterization_state.polygonMode = VK_POLYGON_MODE_POINT;
      break;
    default:
      assert_unhandled_case(description.polygon_mode);
      return false;
  }
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  if (description.cull_front) {
    rasterization_state.cullMode |= VK_CULL_MODE_FRONT_BIT;
  }
  if (description.cull_back) {
    rasterization_state.cullMode |= VK_CULL_MODE_BACK_BIT;
  }
  rasterization_state.frontFace =
      description.front_face_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // Depth bias is dynamic (even toggling - pipeline creation is expensive).
  // "If no depth attachment is present, r is undefined" in the depth bias
  // formula, though Z has no effect on anything if a depth attachment is not
  // used (the guest shader can't access Z), enabling only when there's a
  // depth / stencil attachment for correctness.
  rasterization_state.depthBiasEnable =
      (!edram_fragment_shader_interlock && (description.render_pass_key.depth_and_color_used & 0b1))
          ? VK_TRUE
          : VK_FALSE;
  // TODO(Triang3l): Wide lines.
  rasterization_state.lineWidth = 1.0f;

  bool subpass_has_attachments =
      !edram_fragment_shader_interlock && description.render_pass_key.depth_and_color_used != 0;
  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  if (description.rasterizer_discard) {
    // Keep rasterizer-discard pipelines independent from guest MSAA state, as
    // done by D3D12 when rasterization is disabled.
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  } else if (!edram_fragment_shader_interlock && !subpass_has_attachments) {
    // Keep parity with D3D12 host-render-target path, where draws without
    // color/depth attachments must run at 1x sample count.
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  } else if (description.render_pass_key.msaa_samples == xenos::MsaaSamples::k2X &&
             !render_target_cache_.IsMsaa2xSupported(subpass_has_attachments)) {
    // Using sample 0 as 0 and 3 as 1 for 2x instead (not exactly the same
    // sample locations, but still top-left and bottom-right - however, this can
    // be adjusted with custom sample locations).
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    // Keep parity with D3D12 ROV: sample masks are ignored without attachments.
    if (subpass_has_attachments) {
      sample_mask = 0b1001;
      multisample_state.pSampleMask = &sample_mask;
    }
  } else {
    multisample_state.rasterizationSamples =
        VkSampleCountFlagBits(uint32_t(1) << uint32_t(description.render_pass_key.msaa_samples));
  }
  if (description.sample_rate_shading &&
      multisample_state.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT) {
    multisample_state.sampleShadingEnable = VK_TRUE;
    multisample_state.minSampleShading = 1.0f;
  }

  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil_state.pNext = nullptr;
  if (!edram_fragment_shader_interlock) {
    if (description.depth_write_enable ||
        description.depth_compare_op != xenos::CompareFunction::kAlways) {
      depth_stencil_state.depthTestEnable = VK_TRUE;
      depth_stencil_state.depthWriteEnable = description.depth_write_enable ? VK_TRUE : VK_FALSE;
      depth_stencil_state.depthCompareOp =
          VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(description.depth_compare_op));
    }
    if (description.stencil_test_enable) {
      depth_stencil_state.stencilTestEnable = VK_TRUE;
      depth_stencil_state.front.failOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_front_fail_op));
      depth_stencil_state.front.passOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_front_pass_op));
      depth_stencil_state.front.depthFailOp = VkStencilOp(
          uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_front_depth_fail_op));
      depth_stencil_state.front.compareOp = VkCompareOp(
          uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(description.stencil_front_compare_op));
      depth_stencil_state.back.failOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_back_fail_op));
      depth_stencil_state.back.passOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_back_pass_op));
      depth_stencil_state.back.depthFailOp = VkStencilOp(
          uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_back_depth_fail_op));
      depth_stencil_state.back.compareOp = VkCompareOp(
          uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(description.stencil_back_compare_op));
    }
  }

  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  VkPipelineColorBlendAttachmentState color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  if (!edram_fragment_shader_interlock) {
    uint32_t color_rts_used = description.render_pass_key.depth_and_color_used >> 1;
    {
      static const VkBlendFactor kBlendFactorMap[] = {
          VK_BLEND_FACTOR_ZERO,
          VK_BLEND_FACTOR_ONE,
          VK_BLEND_FACTOR_SRC_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
          VK_BLEND_FACTOR_DST_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
          VK_BLEND_FACTOR_SRC_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          VK_BLEND_FACTOR_DST_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
          VK_BLEND_FACTOR_CONSTANT_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
          VK_BLEND_FACTOR_CONSTANT_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
          VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
      };
      // 8 entries for safety since 3 bits from the guest are passed directly.
      static const VkBlendOp kBlendOpMap[] = {VK_BLEND_OP_ADD,
                                              VK_BLEND_OP_SUBTRACT,
                                              VK_BLEND_OP_MIN,
                                              VK_BLEND_OP_MAX,
                                              VK_BLEND_OP_REVERSE_SUBTRACT,
                                              VK_BLEND_OP_ADD,
                                              VK_BLEND_OP_ADD,
                                              VK_BLEND_OP_ADD};
      assert_true(vulkan_device->properties().independentBlend);
      uint32_t color_rts_remaining = color_rts_used;
      uint32_t color_rt_index;
      while (rex::bit_scan_forward(color_rts_remaining, &color_rt_index)) {
        color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
        VkPipelineColorBlendAttachmentState& color_blend_attachment =
            color_blend_attachments[color_rt_index];
        const PipelineRenderTarget& color_rt = description.render_targets[color_rt_index];
        if (color_rt.src_color_blend_factor != PipelineBlendFactor::kOne ||
            color_rt.dst_color_blend_factor != PipelineBlendFactor::kZero ||
            color_rt.color_blend_op != xenos::BlendOp::kAdd ||
            color_rt.src_alpha_blend_factor != PipelineBlendFactor::kOne ||
            color_rt.dst_alpha_blend_factor != PipelineBlendFactor::kZero ||
            color_rt.alpha_blend_op != xenos::BlendOp::kAdd) {
          color_blend_attachment.blendEnable = VK_TRUE;
          color_blend_attachment.srcColorBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.src_color_blend_factor)];
          color_blend_attachment.dstColorBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.dst_color_blend_factor)];
          color_blend_attachment.colorBlendOp = kBlendOpMap[uint32_t(color_rt.color_blend_op)];
          color_blend_attachment.srcAlphaBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.src_alpha_blend_factor)];
          color_blend_attachment.dstAlphaBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.dst_alpha_blend_factor)];
          color_blend_attachment.alphaBlendOp = kBlendOpMap[uint32_t(color_rt.alpha_blend_op)];
        }
        color_blend_attachment.colorWriteMask = VkColorComponentFlags(color_rt.color_write_mask);
      }
    }
    color_blend_state.attachmentCount = 32 - rex::lzcnt(color_rts_used);
    color_blend_state.pAttachments = color_blend_attachments;
  }

  std::array<VkDynamicState, 8> dynamic_states;
  VkPipelineDynamicStateCreateInfo dynamic_state;
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.pNext = nullptr;
  dynamic_state.flags = 0;
  dynamic_state.dynamicStateCount = 0;
  dynamic_state.pDynamicStates = dynamic_states.data();
  // Regardless of whether some of this state actually has any effect on the
  // pipeline, marking all as dynamic because otherwise, binding any pipeline
  // with such state not marked as dynamic will cause the dynamic state to be
  // invalidated (again, even if it has no effect).
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
  if (!edram_fragment_shader_interlock) {
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_DEPTH_BIAS;
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
  }
  if (dynamic_fragment_shading_rate_) {
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR;
  }

  VkPipelineTessellationStateCreateInfo tessellation_state = {};
  if (tessellated) {
    tessellation_state.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tessellation_state.patchControlPoints = creation_arguments.tessellation_patch_control_points;
  }

  VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {};
  VkFormat color_attachment_formats[xenos::kMaxColorRenderTargets] = {};
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && vulkan_device->properties().dynamicRendering;
  if (use_dynamic_rendering) {
    pipeline_rendering_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipeline_rendering_create_info.pNext = nullptr;
    pipeline_rendering_create_info.viewMask = 0;

    uint32_t color_attachment_count = 0;
    const auto& key = description.render_pass_key;
    xenos::ColorRenderTargetFormat color_formats[] = {
        key.color_0_view_format, key.color_1_view_format, key.color_2_view_format,
        key.color_3_view_format};
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (key.depth_and_color_used & (1 << (1 + i))) {
        color_attachment_formats[i] = render_target_cache_.GetColorVulkanFormat(color_formats[i]);
        color_attachment_count = i + 1;
      }
    }
    pipeline_rendering_create_info.colorAttachmentCount = color_attachment_count;
    pipeline_rendering_create_info.pColorAttachmentFormats =
        color_attachment_count ? color_attachment_formats : nullptr;

    if (key.depth_and_color_used & 0b1) {
      VkFormat depth_format = render_target_cache_.GetDepthVulkanFormat(key.depth_format);
      pipeline_rendering_create_info.depthAttachmentFormat = depth_format;
      pipeline_rendering_create_info.stencilAttachmentFormat = depth_format;
    }
  }

  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = use_dynamic_rendering ? &pipeline_rendering_create_info : nullptr;
  // vulkan_fragment_shading_rate (diagnostics): coarse shading for every guest draw.
  VkPipelineFragmentShadingRateStateCreateInfoKHR fragment_shading_rate_state = {
      VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR};
  if (vulkan_device->extensions().ext_KHR_fragment_shading_rate && !dynamic_fragment_shading_rate_) {
    const std::string& rate = REXCVAR_GET(vulkan_fragment_shading_rate);
    uint32_t rate_w = 1, rate_h = 1;
    if (rate.size() == 3 && rate[1] == 'x') {
      rate_w = std::clamp(uint32_t(rate[0] - '0'), uint32_t(1), uint32_t(4));
      rate_h = std::clamp(uint32_t(rate[2] - '0'), uint32_t(1), uint32_t(4));
    }
    fragment_shading_rate_state.fragmentSize = {rate_w, rate_h};
    fragment_shading_rate_state.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    fragment_shading_rate_state.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    fragment_shading_rate_state.pNext = const_cast<void*>(pipeline_create_info.pNext);
    pipeline_create_info.pNext = &fragment_shading_rate_state;
  }
  pipeline_create_info.flags = 0;
  const bool capture_statistics =
      vulkan_device->extensions().ext_KHR_pipeline_executable_properties;
  if (capture_statistics) {
    pipeline_create_info.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
  }
  bool capture_ir = false;
  if (capture_statistics && creation_arguments.pixel_shader &&
      !REXCVAR_GET(vulkan_pipeline_ir_dump_ps).empty()) {
    const std::string ps_hash =
        fmt::format("{:016X}", creation_arguments.pixel_shader->shader().ucode_data_hash());
    capture_ir = REXCVAR_GET(vulkan_pipeline_ir_dump_ps).find(ps_hash) != std::string::npos;
    if (capture_ir) {
      pipeline_create_info.flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    }
  }
  pipeline_create_info.stageCount = shader_stage_count;
  pipeline_create_info.pStages = shader_stages.data();
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState = tessellated ? &tessellation_state : nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = &depth_stencil_state;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  if (creation_arguments.pipeline_layout == nullptr) {
    return false;
  }
  pipeline_create_info.layout = creation_arguments.pipeline_layout->GetPipelineLayout();
  pipeline_create_info.renderPass =
      use_dynamic_rendering ? VK_NULL_HANDLE : creation_arguments.render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkPipeline pipeline;
  VkResult create_result = dfn.vkCreateGraphicsPipelines(
      device, vulkan_device->pipeline_cache(), 1, &pipeline_create_info, nullptr, &pipeline);
  MarkDriverPipelineCacheUsed();
  if (create_result != VK_SUCCESS) {
    uint64_t ps_hash = creation_arguments.pixel_shader
                           ? creation_arguments.pixel_shader->shader().ucode_data_hash()
                           : 0;
    REXGPU_ERROR(
        "VulkanPipelineCache: vkCreateGraphicsPipelines failed (result={}, vs={:016X}, "
        "ps={:016X}, topo={}, geom={}, tess_mode={}, patch_cp={}, render_pass_key=0x{:08X}, "
        "dynamic_rendering={})",
        int32_t(create_result), creation_arguments.vertex_shader->shader().ucode_data_hash(),
        ps_hash, uint32_t(description.primitive_topology), uint32_t(description.geometry_shader),
        uint32_t(description.tessellation_mode),
        creation_arguments.tessellation_patch_control_points, description.render_pass_key.key,
        uint32_t(use_dynamic_rendering));
    return false;
  }
  if (capture_statistics && !creating_placeholder) {
    // vulkan_pipeline_statistics: one line per pipeline with every executable's
    // integer statistics (the names are the driver's).
    VkPipelineInfoKHR pipeline_info = {VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pipeline_info.pipeline = pipeline;
    uint32_t executable_count = 0;
    dfn.vkGetPipelineExecutablePropertiesKHR(device, &pipeline_info, &executable_count, nullptr);
    std::vector<VkPipelineExecutablePropertiesKHR> executables(
        executable_count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    dfn.vkGetPipelineExecutablePropertiesKHR(device, &pipeline_info, &executable_count,
                                             executables.data());
    std::string statistics_info;
    for (uint32_t i = 0; i < executable_count; ++i) {
      VkPipelineExecutableInfoKHR executable_info = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
      executable_info.pipeline = pipeline;
      executable_info.executableIndex = i;
      uint32_t statistic_count = 0;
      dfn.vkGetPipelineExecutableStatisticsKHR(device, &executable_info, &statistic_count, nullptr);
      std::vector<VkPipelineExecutableStatisticKHR> statistics(
          statistic_count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
      dfn.vkGetPipelineExecutableStatisticsKHR(device, &executable_info, &statistic_count,
                                               statistics.data());
      statistics_info += fmt::format(" | {}:", executables[i].name);
      if (capture_ir) {
        uint32_t ir_count = 0;
        dfn.vkGetPipelineExecutableInternalRepresentationsKHR(device, &executable_info, &ir_count,
                                                              nullptr);
        std::vector<VkPipelineExecutableInternalRepresentationKHR> irs(
            ir_count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
        dfn.vkGetPipelineExecutableInternalRepresentationsKHR(device, &executable_info, &ir_count,
                                                              irs.data());
        std::vector<std::vector<char>> ir_text(ir_count);
        for (uint32_t j = 0; j < ir_count; ++j) {
          ir_text[j].resize(irs[j].dataSize + 1, '\0');
          irs[j].pData = ir_text[j].data();
        }
        dfn.vkGetPipelineExecutableInternalRepresentationsKHR(device, &executable_info, &ir_count,
                                                              irs.data());
        for (uint32_t j = 0; j < ir_count; ++j) {
          REXGPU_INFO("Pipeline IR PS {:016X} executable '{}' representation '{}':\n{}",
                      creation_arguments.pixel_shader->shader().ucode_data_hash(),
                      executables[i].name, irs[j].name, ir_text[j].data());
        }
      }
      for (const VkPipelineExecutableStatisticKHR& statistic : statistics) {
        switch (statistic.format) {
          case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
            statistics_info += fmt::format(" {}={}", statistic.name, statistic.value.b32);
            break;
          case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
            statistics_info += fmt::format(" {}={}", statistic.name, statistic.value.i64);
            break;
          case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
            statistics_info += fmt::format(" {}={}", statistic.name, statistic.value.u64);
            break;
          case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
            statistics_info += fmt::format(" {}={:.2f}", statistic.name, statistic.value.f64);
            break;
          default:
            break;
        }
      }
    }
    REXGPU_INFO("Pipeline statistics VS {:016X} PS {:016X}{}",
                creation_arguments.vertex_shader->shader().ucode_data_hash(),
                creation_arguments.pixel_shader
                    ? creation_arguments.pixel_shader->shader().ucode_data_hash()
                    : 0,
                statistics_info);
  }
  bool was_placeholder =
      creation_arguments.pipeline->second.is_placeholder.load(std::memory_order_acquire);
  VkPipeline old_pipeline =
      creation_arguments.pipeline->second.pipeline.exchange(pipeline, std::memory_order_acq_rel);
  creation_arguments.pipeline->second.pipeline_layout.store(creation_arguments.pipeline_layout,
                                                            std::memory_order_release);
  if (creating_placeholder) {
    creation_arguments.pipeline->second.is_placeholder.store(true, std::memory_order_release);
  } else {
    creation_arguments.pipeline->second.is_placeholder.store(false, std::memory_order_release);
    if (was_placeholder && old_pipeline != VK_NULL_HANDLE) {
      std::lock_guard<std::mutex> lock(deferred_destroy_lock_);
      deferred_destroy_pipelines_.emplace_back(command_processor_.GetCurrentSubmission(),
                                               old_pipeline);
    }
  }

  return true;
}

#if defined(__ANDROID__)
// 1 once gameplay started (set by the game library); the boot precompile's progress in permille
// over the shaders used in play while they compile, -2 once they are done and the rest compiles
// in the background, else -1 (for the app's on-screen notice). Defined in the runtime
// (xboxkrnl_threading.cpp) so that both libraries link them.
extern "C" volatile int rex_game_interactive;
extern "C" volatile int rex_precompile_permille;
extern "C" volatile int rex_precompile_all_permille;
namespace {
// Pipeline creation (the driver's shader compiler) runs in bursts when the game reaches new
// areas or battles. The Android app pins every thread to the middle cluster so that the game's
// threads keep steady frame pacing; compile threads sharing those 4 cores at normal priority
// stretched the game's frames during a burst. They now run at a lower priority (the game's threads
// win any contended core, the compiles use the idle time) and may also use the prime core, but
// not the slowest cluster (where a compile takes several times longer).
// little_cores: run on the slowest cluster only (the creation threads during gameplay, so first-visit
// compiles don't take the cores the game's main / GPU / worker threads run on); otherwise on the
// faster clusters (the boot precompile at the title, where speed matters and the game is idle).
void LowerPipelineCreationThreadPriority(bool little_cores = false) {
  setpriority(PRIO_PROCESS, 0, 10);  // The calling thread only on Linux.
  long cpu_count = sysconf(_SC_NPROCESSORS_CONF);
  if (cpu_count <= 0 || cpu_count > CPU_SETSIZE) {
    return;
  }
  std::vector<uint32_t> max_khz(size_t(cpu_count), 0);
  uint32_t slowest = UINT32_MAX, fastest = 0;
  for (long cpu = 0; cpu < cpu_count; ++cpu) {
    std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                       "/cpufreq/cpuinfo_max_freq";
    if (FILE* file = fopen(path.c_str(), "r")) {
      unsigned int khz = 0;
      if (fscanf(file, "%u", &khz) == 1) {
        max_khz[size_t(cpu)] = khz;
        slowest = std::min(slowest, uint32_t(khz));
        fastest = std::max(fastest, uint32_t(khz));
      }
      fclose(file);
    }
  }
  if (!fastest || slowest == fastest) {
    return;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  for (long cpu = 0; cpu < cpu_count; ++cpu) {
    if (little_cores ? max_khz[size_t(cpu)] == slowest : max_khz[size_t(cpu)] > slowest) {
      CPU_SET(int(cpu), &set);
    }
  }
  if (CPU_COUNT(&set)) {
    sched_setaffinity(0, sizeof(set), &set);
  }
}
}  // namespace
#endif

void VulkanPipelineCache::PrecompileThread() {
#if defined(__ANDROID__)
  LowerPipelineCreationThreadPriority();
#endif
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& properties = vulkan_device->properties();
  // Skip when this driver build already got the whole set (marker: UUID + shader count).
  // Marker: driver pipeline cache UUID, shader count, precompile variant set version.
  uint8_t marker[28];
  std::memcpy(marker, properties.pipelineCacheUUID, 16);
  // Only the shaders with offline-scan metadata count: the others are skipped below, and new ones
  // join the storage during play, which would rerun the precompile (and show the title notice) on
  // the next launch for nothing. This set changes with an app update that ships new metadata.
  uint64_t shader_count = 0;
  for (const PrecompileShader& entry : precompile_pixel_shaders_) {
    shader_count += pixel_shader_meta_.count(entry.hash);
  }
  // Stored vertex shaders (vulkan_precompile_vertex_libraries) in the high half: new ones are rare
  // after the offline scan, and a rerun with everything built only checks the library keys.
  shader_count |= uint64_t(precompile_vertex_shaders_.size()) << 32;
  std::memcpy(marker + 16, &shader_count, 8);
  // 2: 1x only (the game draws its pixel shaders at 1x). 3+: the translation switches are part of
  // the marker - a change rebuilds the libraries here, in the background, instead of compiling
  // them while the game runs (the driver cache is keyed by the SPIR-V).
  // 4: fragment library keys include the translation switches and texture sign classes.
  // 5: vertex (pre-rasterization) libraries too, with the translation switches in their keys.
  uint32_t precompile_version = 5 | (uint32_t(REXCVAR_GET(spirv_texture_sign_branch)) << 8) |
                                (uint32_t(SpirvShaderTranslator::IsTextureSignSpecializationEnabled())
                                 << 12) |
                                (uint32_t(REXCVAR_GET(vulkan_shader_xenos_multiply)) << 9) |
                                (uint32_t(REXCVAR_GET(vulkan_shader_xenos_multiply_vertex)) << 10) |
                                (uint32_t(REXCVAR_GET(vulkan_texture_fetch_implicit_lod)) << 11) |
                                (uint32_t(REXCVAR_GET(vulkan_shared_memory_texel_buffer)) << 13) |
                                (uint32_t(REXCVAR_GET(spirv_structured_forward_jumps)) << 14);
  std::memcpy(marker + 24, &precompile_version, 4);
  if (FILE* file = rex::filesystem::OpenFile(precompile_marker_path_, "rb")) {
    uint8_t existing[28];
    bool done = fread(existing, sizeof(existing), 1, file) == 1 &&
                !std::memcmp(existing, marker, sizeof(marker));
    fclose(file);
    if (done) {
      REXGPU_INFO("VulkanPipelineCache: pipeline libraries for {} pixel shaders already "
                  "precompiled for this driver ({} vertex shaders)",
                  uint32_t(shader_count), uint32_t(shader_count >> 32));
      precompile_pixel_shaders_.clear();
      return;
    }
  }
  auto start = std::chrono::steady_clock::now();
  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  std::atomic<size_t> next_index(0);
  std::atomic<uint32_t> compiled(0), skipped(0), failed(0), already(0);
  std::atomic<uint32_t> vertex_compiled(0), vertex_skipped(0), vertex_failed(0),
      vertex_already(0);
  std::atomic<bool> moved_logged(false), stopped_in_game(false);
  const size_t precompile_total = precompile_pixel_shaders_.size();
  // Work order: the pixel shaders used in play, the vertex variants used in play, the other pixel
  // shaders, the other vertex shaders (canonical variants).
  const size_t vertex_total = precompile_vertex_variants_.size();
  const size_t vertex_used_total = std::min(precompile_vertex_used_count_, vertex_total);
  const size_t work_total = precompile_total + vertex_total;
  // The shaders used in play (sorted first): progress and "ready" for the app cover only them,
  // the rest continues in the background (or after an interruption, at later launches).
  const size_t priority_total =
      precompile_priority_count_ ? std::min(precompile_priority_count_, precompile_total)
                                 : precompile_total;
  std::atomic<size_t> priority_done(0);
  const std::function<void()> priority_item_done = [&]() {
    size_t done = priority_done.fetch_add(1) + 1;
#if defined(__ANDROID__)
    rex_precompile_permille = done >= priority_total ? -2 : int(done * 1000 / priority_total);
#endif
    if (done == priority_total) {
      // Save them right away (the regular schedule waits up to 30 s on a cold start, 3 min with a
      // big cache): a player who starts the game and later swipes the app away (killed, no save at
      // exit) would otherwise lose them and rebuild them at the next launch.
      MarkDriverPipelineCacheUsed();
      {
        // Under the lock so the storage writer can't miss the wake-up between computing its
        // (longer) wait and starting it.
        std::lock_guard<std::mutex> lock(storage_write_request_lock_);
        driver_pipeline_cache_save_soon_.store(true, std::memory_order_relaxed);
      }
      storage_write_request_cond_.notify_all();
      REXGPU_INFO("VulkanPipelineCache: pixel shader libraries for the {} shaders used in play "
                  "ready after {:.1f} s ({} new)",
                  priority_total,
                  double(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count()) /
                      1000.0,
                  compiled.load());
    }
  };
  struct PriorityGuard {
    const std::function<void()>* on_done;
    ~PriorityGuard() {
      if (on_done) (*on_done)();
    }
  };
#if defined(__ANDROID__)
  rex_precompile_permille = 0;
  rex_precompile_all_permille = 0;
  const int in_game_mode = REXCVAR_GET(vulkan_precompile_in_game);
#endif
  auto worker = [&]() {
#if defined(__ANDROID__)
    // Gameplay started before the precompile finished (the first launch after an update or a
    // driver change, when the player does not wait at the title): the 4 compile threads on the
    // middle and prime cores competed with the game's main, GPU and worker threads there (48% of
    // the warp bench's frames over 45 ms instead of 2%). Mode 1 finishes on the slowest cluster
    // only (7.0%), mode 2 (default) stops (4.1-5.6%; the next launch resumes at the title, where
    // the app shows the progress; finished libraries are kept). What remains is the on-demand
    // compiles of the libraries the area needs on the creation threads (~20% of all CPU samples).
    bool moved_to_little = false;
#endif
    SpirvShaderTranslator translator(
        SpirvShaderTranslator::Features(vulkan_device),
        render_target_cache_.msaa_2x_attachments_supported(),
        render_target_cache_.msaa_2x_no_attachments_supported(), edram_fragment_shader_interlock,
        render_target_cache_.draw_resolution_scale_x(),
        render_target_cache_.draw_resolution_scale_y());
    string::StringBuffer ucode_disasm_buffer;
    VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    while (!precompile_shutdown_) {
#if defined(__ANDROID__)
      if (!moved_to_little && rex_game_interactive && in_game_mode) {
        moved_to_little = true;
        if (in_game_mode == 1) {
          LowerPipelineCreationThreadPriority(true);
        } else {
          stopped_in_game = true;
        }
        if (!moved_logged.exchange(true)) {
          REXGPU_INFO("VulkanPipelineCache: gameplay started during the library precompile ({} of "
                      "{} done): {}",
                      next_index.load(), precompile_total,
                      in_game_mode == 1 ? "the compile threads move to the slowest cores"
                                        : "stopped until the next launch");
        }
      }
      if (stopped_in_game) {
        break;
      }
#endif
      size_t index = next_index.fetch_add(1);
      if (index >= work_total) {
        break;
      }
#if defined(__ANDROID__)
      rex_precompile_all_permille = int(index * 1000 / work_total);
#endif
      // Counts this entry towards the used-in-play set when the iteration ends (any path).
      PriorityGuard priority_guard{index < priority_total ? &priority_item_done : nullptr};
      if (index && index % 1000 == 0) {
        REXGPU_INFO("VulkanPipelineCache: precompiling shader libraries, {} of {} ({} pixel, {} "
                    "vertex new)",
                    index, work_total, compiled.load(), vertex_compiled.load());
      }
      // Map the work index (see the work order above).
      bool vertex_item = false;
      size_t item = index;
      if (item >= priority_total) {
        item -= priority_total;
        if (item < vertex_used_total) {
          vertex_item = true;
        } else {
          item -= vertex_used_total;
          if (item < precompile_total - priority_total) {
            item += priority_total;
          } else {
            item = item - (precompile_total - priority_total) + vertex_used_total;
            vertex_item = true;
          }
        }
      }
      if (vertex_item) {
        const PrecompileVertexVariant& variant = precompile_vertex_variants_[item];
        const PrecompileShader& vertex_entry = precompile_vertex_shaders_[variant.shader_index];
        uint64_t modification = variant.modification;
        if (variant.from_storage &&
            IsLibraryAvailable(
                PreRasterizationLibraryKeyHash(vertex_entry.hash, modification, 0, 0))) {
          ++vertex_already;
          continue;
        }
        VulkanShader vertex_shader(vulkan_device, xenos::ShaderType::kVertex, vertex_entry.hash,
                                   vertex_entry.ucode.data(),
                                   uint32_t(vertex_entry.ucode.size()));
        vertex_shader.AnalyzeUcode(ucode_disasm_buffer);
        if (!variant.from_storage) {
          if (vertex_shader.uses_register_dynamic_addressing()) {
            // The register count comes from SQ_PROGRAM_CNTL at draw time.
            ++vertex_skipped;
            continue;
          }
          // The canonical modification the draws request (GetCurrentVertexShaderModification
          // with the canonical interpolator mask): plain vertex shader, every written interpolator,
          // no user clip planes, no point parameters, vertex kill "or" (the register default).
          SpirvShaderTranslator::Modification canonical(translator.GetDefaultVertexShaderModification(
              0, Shader::HostVertexShaderType::kVertex));
          canonical.vertex.interpolator_mask = vertex_shader.writes_interpolators();
          canonical.vertex.vertex_kill_and =
              uint32_t(vulkan_device->properties().shaderCullDistance &&
                       (vertex_shader.writes_point_size_edge_flag_kill_vertex() & 0b100));
          modification = canonical.value;
          if (IsLibraryAvailable(
                  PreRasterizationLibraryKeyHash(vertex_entry.hash, modification, 0, 0))) {
            ++vertex_already;
            continue;
          }
        }
        auto* vertex_translation = static_cast<VulkanShader::VulkanTranslation*>(
            vertex_shader.GetOrCreateTranslation(modification));
        if (!vertex_translation->is_translated() &&
            !TranslateAnalyzedShader(translator, *vertex_translation)) {
          ++vertex_failed;
          continue;
        }
        VkPipelineLayout vertex_layout = command_processor_.GetLibraryLayoutForTextures(
            true, vertex_shader.GetTextureBindingsAfterTranslation().size(),
            vertex_shader.GetSamplerBindingsAfterTranslation().size());
        if (vertex_layout == VK_NULL_HANDLE ||
            vertex_translation->shader_module() == VK_NULL_HANDLE) {
          ++vertex_failed;
          continue;
        }
        if (GetOrCreatePreRasterizationLibrary(vertex_translation, modification, 0, 0,
                                               VK_NULL_HANDLE, vertex_layout,
                                               false) != VK_NULL_HANDLE) {
          ++vertex_compiled;
        } else {
          ++vertex_failed;
        }
        continue;
      }
      index = item;
      const PrecompileShader& entry = precompile_pixel_shaders_[index];
      auto meta_it = pixel_shader_meta_.find(entry.hash);
      if (meta_it == pixel_shader_meta_.end()) {
        ++skipped;
        continue;
      }
      VulkanShader shader(vulkan_device, xenos::ShaderType::kPixel, entry.hash,
                          entry.ucode.data(), uint32_t(entry.ucode.size()));
      shader.AnalyzeUcode(ucode_disasm_buffer);
      if (shader.uses_register_dynamic_addressing()) {
        ++skipped;
        continue;
      }
      // The canonical modification the draws will request (GetCurrentPixelShaderModification and
      // the canonical interpolator masks in IssueDraw).
      uint32_t interpolator_count =
          std::min(xenos::kMaxInterpolators, shader.register_static_address_bound());
      uint32_t interpolator_mask = (UINT32_C(1) << interpolator_count) - 1;
      uint32_t field = meta_it->second;
      uint32_t param_gen_pos = (field >> 8) & 0xFF;
      bool param_gen = (field & 0b10) && param_gen_pos < interpolator_count;
      if (param_gen) {
        interpolator_mask &= ~(UINT32_C(1) << param_gen_pos);
      }
      using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
      DepthStencilMode depth_modes[2];
      uint32_t depth_mode_count = 0;
      if (shader.implicit_early_z_write_allowed()) {
        depth_modes[depth_mode_count++] = DepthStencilMode::kEarlyHint;
        if (shader.writes_color_target(0)) {
          // With alpha test / alpha to coverage.
          depth_modes[depth_mode_count++] = DepthStencilMode::kNoModifiers;
        }
      } else {
        depth_modes[depth_mode_count++] = DepthStencilMode::kNoModifiers;
      }
      for (uint32_t m = 0; m < depth_mode_count; ++m) {
        SpirvShaderTranslator::Modification modification(
            translator.GetDefaultPixelShaderModification(0));
        modification.pixel.interpolator_mask = interpolator_mask;
        modification.pixel.interpolators_centroid = 0;
        if (param_gen) {
          modification.pixel.param_gen_enable = 1;
          modification.pixel.param_gen_interpolator = param_gen_pos;
        }
        modification.pixel.depth_stencil_mode = depth_modes[m];
        uint64_t key_hash = FragmentLibraryKeyHash(entry.hash, modification.value, multisample);
        if (IsLibraryAvailable(key_hash)) {
          ++already;
          continue;
        }
        auto* translation = static_cast<VulkanShader::VulkanTranslation*>(
            shader.GetOrCreateTranslation(modification.value));
        if (!translation->is_translated() && !TranslateAnalyzedShader(translator, *translation)) {
          ++failed;
          continue;
        }
        VkPipelineLayout layout = command_processor_.GetLibraryLayoutForTextures(
            false, shader.GetTextureBindingsAfterTranslation().size(),
            shader.GetSamplerBindingsAfterTranslation().size());
        if (layout == VK_NULL_HANDLE || translation->shader_module() == VK_NULL_HANDLE) {
          ++failed;
          continue;
        }
        if (GetOrCreateFragmentLibrary(key_hash, translation->shader_module(), layout,
                                       multisample) != VK_NULL_HANDLE) {
          ++compiled;
        } else {
          ++failed;
        }
      }
    }
  };
  // A few threads at a low priority, not on the slowest cores (see
  // LowerPipelineCreationThreadPriority): the game's threads win any contended core.
  uint32_t thread_count =
      REXCVAR_GET(vulkan_precompile_threads) > 0
          ? uint32_t(REXCVAR_GET(vulkan_precompile_threads))
          : std::max(uint32_t(1),
                     std::min(uint32_t(4), rex::thread::logical_processor_count() / 2));
  uint32_t little_thread_count = 0;
#if defined(__ANDROID__)
  little_thread_count = uint32_t(REXCVAR_GET(vulkan_precompile_little_threads));
#endif
  std::vector<std::unique_ptr<rex::thread::Thread>> threads;
  for (uint32_t i = 1; i < thread_count + little_thread_count; ++i) {
    const bool little = i >= thread_count;
    threads.push_back(rex::thread::Thread::Create({}, [&worker, little]() {
#if defined(__ANDROID__)
      LowerPipelineCreationThreadPriority(little);
#endif
      worker();
    }));
    threads.back()->set_name("Vulkan Library Precompile");
  }
  thread_count += little_thread_count;
  worker();
  for (const auto& thread : threads) {
    rex::thread::Wait(thread.get(), false);
  }
  bool finished = !precompile_shutdown_ && !stopped_in_game;
#if defined(__ANDROID__)
  rex_precompile_permille = -1;
  rex_precompile_all_permille = -1;
#endif
  REXGPU_INFO("VulkanPipelineCache: pixel shader library precompile {}: {} libraries compiled, "
              "{} shader variants already done, {} shaders skipped (no metadata / dynamic "
              "registers), {} failed, {:.1f} s on {} threads",
              finished ? "finished" : "stopped", compiled.load(), already.load(), skipped.load(),
              failed.load(),
              double(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count()) /
                  1000.0,
              thread_count);
  if (vertex_total) {
    REXGPU_INFO("VulkanPipelineCache: vertex library precompile: {} compiled, {} already done, {} "
                "skipped (dynamic registers), {} failed of {} variants ({} used in play)",
                vertex_compiled.load(), vertex_already.load(), vertex_skipped.load(),
                vertex_failed.load(), vertex_total, vertex_used_total);
  }
  if (finished) {
    if (FILE* file = rex::filesystem::OpenFile(precompile_marker_path_, "wb")) {
      fwrite(marker, sizeof(marker), 1, file);
      fclose(file);
    }
    // The driver cache now holds the libraries: save it soon.
    MarkDriverPipelineCacheUsed();
    driver_pipeline_cache_save_soon_.store(true, std::memory_order_relaxed);
  }
  precompile_pixel_shaders_.clear();
  precompile_pixel_shaders_.shrink_to_fit();
}

bool VulkanPipelineCache::TranslateShadersForCreation(
    PipelineCreationArguments& creation_arguments) {
  VulkanShader::VulkanTranslation* translations[2] = {
      const_cast<VulkanShader::VulkanTranslation*>(creation_arguments.vertex_shader),
      const_cast<VulkanShader::VulkanTranslation*>(creation_arguments.pixel_shader)};
  for (VulkanShader::VulkanTranslation* translation : translations) {
    if (!translation) {
      continue;
    }
    if (!translation->is_translated()) {
      if (!async_shader_translator_) {
        return false;
      }
      std::lock_guard<std::mutex> lock(async_translation_mutex_);
      if (!translation->is_translated()) {
        assert_true(translation->shader().is_ucode_analyzed());
        auto start = std::chrono::steady_clock::now();
        bool translated = TranslateAnalyzedShader(*async_shader_translator_, *translation);
        counter_translations_async_.fetch_add(1, std::memory_order_relaxed);
        counter_translations_async_us_.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count()),
            std::memory_order_relaxed);
        if (!translated) {
          return false;
        }
      }
    }
    if (!translation->is_valid()) {
      return false;
    }
  }
  if (!creation_arguments.pipeline_layout) {
    const auto& vertex_shader =
        static_cast<const VulkanShader&>(creation_arguments.vertex_shader->shader());
    const VulkanShader* pixel_shader =
        creation_arguments.pixel_shader
            ? &static_cast<const VulkanShader&>(creation_arguments.pixel_shader->shader())
            : nullptr;
    creation_arguments.pipeline_layout = command_processor_.GetPipelineLayout(
        pixel_shader ? pixel_shader->GetTextureBindingsAfterTranslation().size() : 0,
        pixel_shader ? pixel_shader->GetSamplerBindingsAfterTranslation().size() : 0,
        vertex_shader.GetTextureBindingsAfterTranslation().size(),
        vertex_shader.GetSamplerBindingsAfterTranslation().size());
    if (!creation_arguments.pipeline_layout) {
      REXGPU_ERROR("VulkanPipelineCache: no pipeline layout for an asynchronously translated "
                   "pipeline (VS {:016X})",
                   vertex_shader.ucode_data_hash());
      return false;
    }
  }
  return true;
}

void VulkanPipelineCache::CreationThread(size_t thread_index) {
#if defined(__ANDROID__)
  LowerPipelineCreationThreadPriority(REXCVAR_GET(vulkan_pipeline_creation_little_cores));
#endif
  while (true) {
    PipelineCreationArguments creation_arguments;
    {
      std::unique_lock<std::mutex> lock(creation_request_lock_);
      if (thread_index >= creation_threads_shutdown_from_ || creation_queue_.empty()) {
        if (creation_completion_set_event_ && creation_threads_busy_ == 0) {
          creation_completion_set_event_ = false;
          creation_completion_event_->Set();
        }
        if (thread_index >= creation_threads_shutdown_from_) {
          return;
        }
        creation_request_cond_.wait(lock);
        continue;
      }
      creation_arguments = creation_queue_.top();
      creation_queue_.pop();
      ++creation_threads_busy_;
    }

    bool created = TranslateShadersForCreation(creation_arguments) &&
                   EnsurePipelineCreated(creation_arguments);
    if (!created) {
      bool has_placeholder =
          creation_arguments.pipeline->second.is_placeholder.load(std::memory_order_acquire);
      VkPipeline pipeline =
          creation_arguments.pipeline->second.pipeline.load(std::memory_order_acquire);
      if (has_placeholder && pipeline != VK_NULL_HANDLE) {
        // Keep the placeholder resident and stop waiting for a real pipeline.
        creation_arguments.pipeline->second.is_placeholder.store(false, std::memory_order_release);
      } else {
        creation_arguments.pipeline->second.pipeline.store(VK_NULL_HANDLE,
                                                           std::memory_order_release);
        // A pending entry (no placeholder pipeline) must not keep its draws skipped forever.
        creation_arguments.pipeline->second.is_placeholder.store(false, std::memory_order_release);
      }
    }

    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      --creation_threads_busy_;
    }
  }
}

void VulkanPipelineCache::CreateQueuedPipelinesOnProcessorThread() {
  assert_false(creation_threads_.empty());
  while (true) {
    PipelineCreationArguments creation_arguments;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      if (creation_queue_.empty()) {
        break;
      }
      creation_arguments = creation_queue_.top();
      creation_queue_.pop();
    }
    bool created = TranslateShadersForCreation(creation_arguments) &&
                   EnsurePipelineCreated(creation_arguments);
    if (!created) {
      bool has_placeholder =
          creation_arguments.pipeline->second.is_placeholder.load(std::memory_order_acquire);
      VkPipeline pipeline =
          creation_arguments.pipeline->second.pipeline.load(std::memory_order_acquire);
      if (has_placeholder && pipeline != VK_NULL_HANDLE) {
        creation_arguments.pipeline->second.is_placeholder.store(false, std::memory_order_release);
      } else {
        creation_arguments.pipeline->second.pipeline.store(VK_NULL_HANDLE,
                                                           std::memory_order_release);
        // A pending entry (no placeholder pipeline) must not keep its draws skipped forever.
        creation_arguments.pipeline->second.is_placeholder.store(false, std::memory_order_release);
      }
    }
  }
}

void VulkanPipelineCache::ProcessDeferredPipelineDestructions(bool force_all) {
  std::vector<VkPipeline> pipelines_to_destroy;
  uint64_t completed_submission = command_processor_.GetCompletedSubmission();
  {
    std::lock_guard<std::mutex> lock(deferred_destroy_lock_);
    if (deferred_destroy_pipelines_.empty()) {
      return;
    }
    auto it = deferred_destroy_pipelines_.begin();
    while (it != deferred_destroy_pipelines_.end()) {
      if (force_all || it->first <= completed_submission) {
        pipelines_to_destroy.push_back(it->second);
        it = deferred_destroy_pipelines_.erase(it);
      } else if (!force_all) {
        // The queue is sorted by submission index.
        break;
      } else {
        ++it;
      }
    }
  }
  if (pipelines_to_destroy.empty()) {
    return;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (VkPipeline pipeline : pipelines_to_destroy) {
    if (pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
}

void VulkanPipelineCache::StorageWriteThread() {
  ShaderStoredHeader shader_header;
  // Don't leak anything in unused bits.
  std::memset(&shader_header, 0, sizeof(shader_header));

  std::vector<uint32_t> ucode_guest_endian;
  ucode_guest_endian.reserve(0xFFFF);

  bool flush_shaders = false;
  bool flush_pipelines = false;
  bool save_driver_pipeline_cache = false;

  while (true) {
    if (flush_shaders) {
      flush_shaders = false;
      assert_not_null(shader_storage_file_);
      FlushAndSyncStorageFile(shader_storage_file_);
    }
    if (flush_pipelines) {
      flush_pipelines = false;
      assert_not_null(pipeline_storage_file_);
      FlushAndSyncStorageFile(pipeline_storage_file_);
    }
    if (save_driver_pipeline_cache) {
      save_driver_pipeline_cache = false;
      SaveDriverPipelineCache();
    }

    const Shader* shader = nullptr;
    PipelineStoredDescription pipeline_description;
    bool write_pipeline = false;
    {
      std::unique_lock<std::mutex> lock(storage_write_request_lock_);
      if (storage_write_thread_shutdown_) {
        return;
      }
      if (!storage_write_shader_queue_.empty()) {
        shader = storage_write_shader_queue_.front();
        storage_write_shader_queue_.pop_front();
      } else if (storage_write_flush_shaders_) {
        storage_write_flush_shaders_ = false;
        flush_shaders = true;
      }
      if (!storage_write_pipeline_queue_.empty()) {
        std::memcpy(&pipeline_description, &storage_write_pipeline_queue_.front(),
                    sizeof(pipeline_description));
        storage_write_pipeline_queue_.pop_front();
        write_pipeline = true;
      } else if (storage_write_flush_pipelines_) {
        storage_write_flush_pipelines_ = false;
        flush_pipelines = true;
      }
      if (!shader && !write_pipeline) {
        if (flush_shaders || flush_pipelines) {
          // Flush now rather than after the next request: the app may be killed at any time,
          // losing whatever is still in the stdio buffers.
          continue;
        }
        int64_t driver_pipeline_cache_save_delay_ms = GetDriverPipelineCacheSaveDelayMs();
        if (driver_pipeline_cache_save_delay_ms == 0) {
          save_driver_pipeline_cache = true;
        } else if (driver_pipeline_cache_save_delay_ms > 0) {
          storage_write_request_cond_.wait_for(
              lock, std::chrono::milliseconds(driver_pipeline_cache_save_delay_ms));
        } else {
          storage_write_request_cond_.wait(lock);
        }
        continue;
      }
    }

    if (shader) {
      shader_header.ucode_data_hash = shader->ucode_data_hash();
      shader_header.ucode_dword_count = shader->ucode_dword_count();
      shader_header.type = shader->type();
      assert_not_null(shader_storage_file_);
      fwrite(&shader_header, sizeof(shader_header), 1, shader_storage_file_);
      if (shader_header.ucode_dword_count) {
        ucode_guest_endian.resize(shader_header.ucode_dword_count);
        // Need to swap because the hash is calculated for the shader with guest
        // endianness.
        memory::copy_and_swap(ucode_guest_endian.data(), shader->ucode_dwords(),
                              shader_header.ucode_dword_count);
        fwrite(ucode_guest_endian.data(), shader_header.ucode_dword_count * sizeof(uint32_t), 1,
               shader_storage_file_);
      }
    }

    if (write_pipeline) {
      assert_not_null(pipeline_storage_file_);
      fwrite(&pipeline_description, sizeof(pipeline_description), 1, pipeline_storage_file_);
    }
  }
}

}  // namespace rex::graphics::vulkan
