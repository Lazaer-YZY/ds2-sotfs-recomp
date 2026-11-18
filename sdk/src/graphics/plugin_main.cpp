/**
 * @file        graphics/plugin_main.cpp
 * @brief       rexgpu-xenos plugin entry points
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <cstdint>
#include <string_view>

#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

namespace {
std::atomic<rex::graphics::GraphicsSystem*> g_plugin_graphics_system{nullptr};
}  // namespace

// The game's Settings > Anti-Aliasing (src/lr_hooks.cpp lr_apply_aa): 0 off, 1 FXAA, 2 FXAA
// extreme, from the next frame. Returns 0 while the renderer is not up yet (the game retries).
extern "C" REX_GPU_PLUGIN_EXPORT int32_t lr_set_post_effect(int32_t choice) {
  rex::graphics::GraphicsSystem* graphics_system = g_plugin_graphics_system.load();
  if (!graphics_system || !graphics_system->command_processor()) {
    return 0;
  }
  using SwapPostEffect = rex::graphics::CommandProcessor::SwapPostEffect;
  graphics_system->command_processor()->RequestSwapPostEffect(
      choice == 2 ? SwapPostEffect::kFxaaExtreme
                  : choice == 1 ? SwapPostEffect::kFxaa : SwapPostEffect::kNone);
  return 1;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    REXLOG_ERROR("rexgpu-xenos: host requested ABI {}, plugin is ABI {}", abi_version,
                 rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-xenos: invalid GpuCreateInfo");
    return nullptr;
  }

  std::string_view backend = info->backend ? info->backend : "any";
#if REX_HAS_D3D12
  if (backend == "any" || backend == "d3d12") {
    return new rex::graphics::d3d12::D3D12GraphicsSystem();
  }
#endif
#if REX_HAS_VULKAN
  if (backend == "any" || backend == "vulkan") {
    auto* vulkan_graphics_system = new rex::graphics::vulkan::VulkanGraphicsSystem();
    g_plugin_graphics_system.store(vulkan_graphics_system);
    return vulkan_graphics_system;
  }
#endif
  REXLOG_ERROR("rexgpu-xenos: requested backend '{}' is not compiled into this plugin", backend);
  return nullptr;
}
