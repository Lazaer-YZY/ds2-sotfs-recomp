/**
 * @file        system/gpu_plugin_loader.cpp
 * @brief       Host-side loader for GPU emulation plugin DLLs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/gpu_plugin.h>

#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/dynlib.h>

#if REX_PLATFORM_ANDROID
#include <dlfcn.h>
#endif

REXCVAR_DEFINE_STRING(gpu_plugin_dir, "", "Runtime",
                      "Directory to load GPU plugins from before the executable folder "
                      "(empty = executable folder only; Android also tries the app's "
                      "native library directory first)");

namespace rex::system {

namespace {

// Plugin binaries follow the SDK's per-config postfix convention; this TU is
// part of rexruntime, so REXGLUE_BUILD_CONFIG matches the plugin's config.
std::string PluginFileName(std::string_view name) {
  constexpr std::string_view kConfig = REXGLUE_BUILD_CONFIG;
  std::string_view postfix = "";
  if (kConfig == "Debug") {
    postfix = "d";
  } else if (kConfig == "RelWithDebInfo") {
    postfix = "rd";
  }
#if REX_PLATFORM_WIN32
  return fmt::format("rexgpu-{}{}.dll", name, postfix);
#elif REX_PLATFORM_MAC
  return fmt::format("librexgpu-{}{}.dylib", name, postfix);
#else
  return fmt::format("librexgpu-{}{}.so", name, postfix);
#endif
}

// Plugins stay loaded for process lifetime: guest threads may still be in
// plugin code pages at shutdown.
std::vector<platform::DynamicLibrary>& LoadedPlugins() {
  static std::vector<platform::DynamicLibrary> plugins;
  return plugins;
}

}  // namespace


std::unique_ptr<IGraphicsSystem> LoadGpuPlugin(std::string_view name, std::string_view backend) {
  const std::string file_name = PluginFileName(name);
  platform::DynamicLibrary library;
  // Where the plugin came from (for logs); a path or a soname.
  std::filesystem::path path;

#if REX_PLATFORM_ANDROID
  // The "executable" is /system/bin/app_process; the plugin lives in the APK's
  // native library directory and is usually already loaded by SDLActivity.
  // 1) Reuse the loaded copy; 2) let the linker search the app's namespace.
  if (void* loaded = dlopen(file_name.c_str(), RTLD_NOW | RTLD_NOLOAD)) {
    dlclose(loaded);  // Load() below takes its own reference.
    if (library.Load(file_name, platform::SymbolResolution::kImmediate)) {
      path = file_name;
      REXSYS_INFO("GPU plugin '{}': using already loaded {}", name, file_name);
    }
  }
  if (!library && library.Load(file_name, platform::SymbolResolution::kImmediate)) {
    path = file_name;
    REXSYS_INFO("GPU plugin '{}': loaded {} from the app's library search path", name, file_name);
  }
#endif

  // 3) Android: the directory holding librexruntime.so (the native library
  // dir), 4) gpu_plugin_dir (if set), 5) next to the executable (default).
  std::vector<std::filesystem::path> candidates;
#if REX_PLATFORM_ANDROID
  if (auto runtime_path = rex::filesystem::GetExecutablePath(); !runtime_path.empty()) {
    candidates.push_back(runtime_path.parent_path() / file_name);
  }
#endif
  if (std::string dir = REXCVAR_GET(gpu_plugin_dir); !dir.empty()) {
    candidates.push_back(std::filesystem::path(dir) / file_name);
  }
  candidates.push_back(rex::filesystem::GetExecutableFolder() / file_name);

  std::filesystem::path last_tried;
  for (size_t i = 0; !library && i < candidates.size(); ++i) {
    last_tried = candidates[i];
    if (!std::filesystem::exists(last_tried)) {
      continue;
    }
    if (!library.Load(last_tried, platform::SymbolResolution::kImmediate)) {
      REXSYS_ERROR("GPU plugin '{}' failed to load: {}", name, last_tried.string());
      return nullptr;
    }
    path = last_tried;
    if (candidates.size() > 1) {
      REXSYS_INFO("GPU plugin '{}': loaded {}", name, path.string());
    }
  }

  if (!library) {
    REXSYS_ERROR(
        "GPU plugin '{}' not found at {}. Stage it next to the executable "
        "(GPU_PLUGINS {} in rexglue_configure_target) or set gpu_plugin_dir.",
        name, last_tried.string(), name);
    return nullptr;
  }

  auto abi_version_fn = library.GetSymbol<GpuAbiVersionFn>(kGpuAbiVersionSymbol);
  auto create_fn = library.GetSymbol<GpuCreateFn>(kGpuCreateSymbol);
  if (!abi_version_fn || !create_fn) {
    REXSYS_ERROR("GPU plugin '{}' is not a rexglue GPU plugin (missing {} / {} exports): {}", name,
                 kGpuAbiVersionSymbol, kGpuCreateSymbol, path.string());
    return nullptr;
  }

  uint32_t plugin_abi = abi_version_fn();
  if (plugin_abi != kGpuPluginAbiVersion) {
    REXSYS_ERROR("GPU plugin '{}' has ABI version {}, host expects {}: {}", name, plugin_abi,
                 kGpuPluginAbiVersion, path.string());
    return nullptr;
  }

  std::string backend_str(backend);
  GpuCreateInfo info{};
  info.struct_size = sizeof(GpuCreateInfo);
  info.backend = backend_str.c_str();

  IGraphicsSystem* graphics_system = create_fn(kGpuPluginAbiVersion, &info);
  if (!graphics_system) {
    REXSYS_ERROR("GPU plugin '{}' factory returned no graphics system (backend '{}')", name,
                 backend_str);
    return nullptr;
  }

  LoadedPlugins().push_back(std::move(library));
  REXSYS_DEBUG("GPU plugin '{}' loaded ({})", name, path.filename().string());
  return std::unique_ptr<IGraphicsSystem>(graphics_system);
}


}  // namespace rex::system
