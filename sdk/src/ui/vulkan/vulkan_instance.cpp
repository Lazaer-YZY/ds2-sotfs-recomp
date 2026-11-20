/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/loader_override.h>
#include <rex/ui/vulkan/presenter.h>

#if REX_PLATFORM_MAC
#include "vulkan_moltenvk.h"
#endif

REXCVAR_DEFINE_BOOL(vulkan_log_debug_messages, true, "UI/Vulkan", "Log Vulkan debug messages");
REXCVAR_DEFINE_STRING(vulkan_emulate_gpu, "", "UI/Vulkan",
                      "Testing on a GPU that has everything: hide the Vulkan features and formats another GPU "
                      "family lacks, so the renderer takes that family's fallback paths here. \"mali\": Arm's "
                      "Mali driver (no vertex-stage stores, clip or cull distances, non-solid fill, depth clamp, "
                      "dual-source blending, logic ops, multiple viewports, sparse memory, BC textures, "
                      "mirror-once sampling; 16-bit normalized formats not renderable). \"mali_vk11\": the same "
                      "on a Vulkan 1.1 driver (older Mali phones). \"panvk\": \"mali\" and no geometry or "
                      "tessellation shaders (Mesa PanVK). Only the renderer's choices are "
                      "emulated, not the other driver's behaviour or speed.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(vulkan_emulate_gpu_keep, "", "UI/Vulkan",
                      "With vulkan_emulate_gpu: feature groups to leave visible, to find which one a difference "
                      "comes from (comma list: norm16 = 16-bit normalized render targets, bc, clip = clip and cull "
                      "distances, depthclamp, mirror = mirror-once sampling, stores = vertex-stage stores, misc = "
                      "non-solid fill, dual-source blending, logic ops, multiple viewports)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex {
namespace ui {
namespace vulkan {

namespace {
// vulkan_emulate_gpu: the instance's physical device queries, filtered. Features come from Vulkan hardware database
// reports of Arm's Mali driver (r25-r50, 2026; depth clamp is missing on the older ones, so it is hidden too);
// PanVK has no geometry shaders (its geometry shader support was still being written in 2026). Which 16-bit
// normalized formats Mali can render to is not known here: they are hidden to take the fallback.
enum class EmulatedGpu { kNone, kMali, kMaliVk11, kPanVk };
EmulatedGpu g_emulated_gpu = EmulatedGpu::kNone;
PFN_vkEnumerateDeviceExtensionProperties g_real_enumerate_device_extensions = nullptr;
PFN_vkGetPhysicalDeviceProperties g_real_get_properties = nullptr;
PFN_vkGetPhysicalDeviceProperties2 g_real_get_properties_2 = nullptr;
PFN_vkGetPhysicalDeviceFeatures g_real_get_features = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 g_real_get_features_2 = nullptr;
PFN_vkGetPhysicalDeviceFormatProperties g_real_get_format_properties = nullptr;
// vulkan_emulate_gpu_keep: groups left visible.
bool g_keep_norm16 = false, g_keep_bc = false, g_keep_clip = false, g_keep_depth_clamp = false, g_keep_mirror = false,
     g_keep_stores = false, g_keep_misc = false;

void HideEmulatedFeatures(VkPhysicalDeviceFeatures& features) {
  if (!g_keep_stores) features.vertexPipelineStoresAndAtomics = VK_FALSE;
  if (!g_keep_clip) {
    features.shaderClipDistance = VK_FALSE;
    features.shaderCullDistance = VK_FALSE;
  }
  if (!g_keep_depth_clamp) features.depthClamp = VK_FALSE;
  if (!g_keep_misc) {
    features.fillModeNonSolid = VK_FALSE;
    features.dualSrcBlend = VK_FALSE;
    features.logicOp = VK_FALSE;
    features.multiViewport = VK_FALSE;
  }
  if (!g_keep_bc) features.textureCompressionBC = VK_FALSE;
  features.sparseBinding = VK_FALSE;
  features.sparseResidencyBuffer = VK_FALSE;
  features.sparseResidencyImage2D = VK_FALSE;
  features.sparseResidencyImage3D = VK_FALSE;
  features.sparseResidencyAliased = VK_FALSE;
  if (g_emulated_gpu == EmulatedGpu::kPanVk) {
    features.geometryShader = VK_FALSE;
    features.tessellationShader = VK_FALSE;
  }
}

VKAPI_ATTR void VKAPI_CALL EmulatedGetFeatures(VkPhysicalDevice physical_device,
                                               VkPhysicalDeviceFeatures* features) {
  g_real_get_features(physical_device, features);
  HideEmulatedFeatures(*features);
}

VKAPI_ATTR void VKAPI_CALL EmulatedGetFeatures2(VkPhysicalDevice physical_device,
                                                VkPhysicalDeviceFeatures2* features) {
  g_real_get_features_2(physical_device, features);
  HideEmulatedFeatures(features->features);
  for (auto* next = static_cast<VkBaseOutStructure*>(features->pNext); next; next = next->pNext) {
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES && !g_keep_mirror) {
      auto* features_1_2 = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(next);
      features_1_2->samplerMirrorClampToEdge = VK_FALSE;
      features_1_2->uniformBufferStandardLayout = VK_FALSE;
    }
  }
}

// The extensions Arm's Mali driver lacks among those the renderer looks for (mirror-once sampling, which a Vulkan 1.1
// device would otherwise get from the extension).
VKAPI_ATTR VkResult VKAPI_CALL EmulatedEnumerateDeviceExtensions(VkPhysicalDevice physical_device,
                                                                 const char* layer_name, uint32_t* count,
                                                                 VkExtensionProperties* extensions) {
  uint32_t real_count = 0;
  VkResult result = g_real_enumerate_device_extensions(physical_device, layer_name, &real_count, nullptr);
  if (result != VK_SUCCESS) return result;
  std::vector<VkExtensionProperties> all(real_count);
  result = g_real_enumerate_device_extensions(physical_device, layer_name, &real_count, all.data());
  if (result < 0) return result;
  all.resize(real_count);
  if (!g_keep_mirror) all.erase(std::remove_if(all.begin(), all.end(),
                           [](const VkExtensionProperties& e) {
                             return std::strcmp(e.extensionName, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME) == 0;
                           }),
            all.end());
  if (!extensions) {
    *count = uint32_t(all.size());
    return VK_SUCCESS;
  }
  const uint32_t written = std::min(*count, uint32_t(all.size()));
  std::copy_n(all.begin(), written, extensions);
  *count = written;
  return written < all.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

// mali_vk11: the device reports Vulkan 1.1 (the renderer then uses extensions instead of 1.2 / 1.3 core features).
VKAPI_ATTR void VKAPI_CALL EmulatedGetProperties(VkPhysicalDevice physical_device,
                                                 VkPhysicalDeviceProperties* properties) {
  g_real_get_properties(physical_device, properties);
  properties->apiVersion = std::min(properties->apiVersion, uint32_t(VK_MAKE_API_VERSION(0, 1, 1, 0)));
}

VKAPI_ATTR void VKAPI_CALL EmulatedGetProperties2(VkPhysicalDevice physical_device,
                                                  VkPhysicalDeviceProperties2* properties) {
  g_real_get_properties_2(physical_device, properties);
  properties->properties.apiVersion =
      std::min(properties->properties.apiVersion, uint32_t(VK_MAKE_API_VERSION(0, 1, 1, 0)));
}

VKAPI_ATTR void VKAPI_CALL EmulatedGetFormatProperties(VkPhysicalDevice physical_device, VkFormat format,
                                                       VkFormatProperties* properties) {
  g_real_get_format_properties(physical_device, format, properties);
  if (format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK) {
    if (!g_keep_bc) *properties = {};
    return;
  }
  if (g_keep_norm16) return;
  switch (format) {
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM: {
      constexpr VkFormatFeatureFlags kRender =
          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
      properties->linearTilingFeatures &= ~kRender;
      properties->optimalTilingFeatures &= ~kRender;
      break;
    }
    default:
      break;
  }
}

void InstallGpuEmulation(VulkanInstance::Functions& ifn) {
  const std::string name = REXCVAR_GET(vulkan_emulate_gpu);
  {
    const std::string keep = "," + REXCVAR_GET(vulkan_emulate_gpu_keep) + ",";
    auto has = [&keep](const char* group) { return keep.find(std::string(",") + group + ",") != std::string::npos; };
    g_keep_norm16 = has("norm16");
    g_keep_bc = has("bc");
    g_keep_clip = has("clip");
    g_keep_depth_clamp = has("depthclamp");
    g_keep_mirror = has("mirror");
    g_keep_stores = has("stores");
    g_keep_misc = has("misc");
  }
  if (name.empty()) return;
  if (name == "mali") {
    g_emulated_gpu = EmulatedGpu::kMali;
  } else if (name == "mali_vk11") {
    g_emulated_gpu = EmulatedGpu::kMaliVk11;
  } else if (name == "panvk") {
    g_emulated_gpu = EmulatedGpu::kPanVk;
  } else {
    REXLOG_WARN("vulkan_emulate_gpu: unknown GPU family '{}' (mali, mali_vk11, panvk); nothing hidden", name);
    return;
  }
  if (g_emulated_gpu == EmulatedGpu::kMaliVk11) {
    g_real_get_properties = ifn.vkGetPhysicalDeviceProperties;
    ifn.vkGetPhysicalDeviceProperties = EmulatedGetProperties;
    if (ifn.vkGetPhysicalDeviceProperties2) {
      g_real_get_properties_2 = ifn.vkGetPhysicalDeviceProperties2;
      ifn.vkGetPhysicalDeviceProperties2 = EmulatedGetProperties2;
    }
  }
  g_real_get_features = ifn.vkGetPhysicalDeviceFeatures;
  ifn.vkGetPhysicalDeviceFeatures = EmulatedGetFeatures;
  if (ifn.vkGetPhysicalDeviceFeatures2) {
    g_real_get_features_2 = ifn.vkGetPhysicalDeviceFeatures2;
    ifn.vkGetPhysicalDeviceFeatures2 = EmulatedGetFeatures2;
  }
  g_real_get_format_properties = ifn.vkGetPhysicalDeviceFormatProperties;
  ifn.vkGetPhysicalDeviceFormatProperties = EmulatedGetFormatProperties;
  g_real_enumerate_device_extensions = ifn.vkEnumerateDeviceExtensionProperties;
  ifn.vkEnumerateDeviceExtensionProperties = EmulatedEnumerateDeviceExtensions;
  REXLOG_WARN("vulkan_emulate_gpu: the renderer sees the features and formats of {} (testing only)",
              g_emulated_gpu == EmulatedGpu::kMali       ? "Arm's Mali driver"
              : g_emulated_gpu == EmulatedGpu::kMaliVk11 ? "Arm's Mali driver, Vulkan 1.1"
                                                         : "Mesa PanVK on Mali");
}
}  // namespace

std::unique_ptr<VulkanInstance> VulkanInstance::Create(const bool with_surface,
                                                       const bool try_enable_validation) {
  std::unique_ptr<VulkanInstance> vulkan_instance(new VulkanInstance());

  // Load the RenderDoc API if connected.

  vulkan_instance->renderdoc_api_ = RenderDocAPI::CreateIfConnected();

  // Load the loader library.

  Functions& ifn = vulkan_instance->functions_;

  bool functions_loaded = true;
  bool loader_loaded = false;
  // Application-provided vkGetInstanceProcAddr (e.g. Turnip via
  // libadrenotools): skip the system loader entirely. vkDestroyInstance is
  // resolved from the instance after it has been created.
  void* const instance_proc_addr_override = GetInstanceProcAddrOverride();
  if (instance_proc_addr_override) {
    ifn.vkGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(instance_proc_addr_override);
    REXLOG_INFO("Vulkan: using application-provided vkGetInstanceProcAddr override");
  } else {
#if REX_PLATFORM_MAC
    const MacOSVulkanRuntimePaths macos_runtime_paths = DetectMacOSVulkanRuntimePaths();
    ConfigureMacOSVulkanEnvironment(macos_runtime_paths);

    std::vector<std::filesystem::path> loader_candidates = macos_runtime_paths.loader_candidates;
    loader_candidates.emplace_back(platform::lib_names::kVulkanLoader);
    loader_candidates.emplace_back("libvulkan.dylib");
    loader_candidates.emplace_back("libMoltenVK.dylib");

    std::vector<std::string> attempted_loader_paths;
    attempted_loader_paths.reserve(loader_candidates.size());
    for (const std::filesystem::path& candidate : loader_candidates) {
      if (candidate.empty()) {
        continue;
      }
      attempted_loader_paths.push_back(candidate.string());
      if (vulkan_instance->loader_.Load(candidate)) {
        loader_loaded = true;
        REXLOG_INFO("Loaded Vulkan runtime from {}", candidate.string());
        break;
      }
    }
    if (!loader_loaded) {
      REXLOG_ERROR("Failed to load a Vulkan runtime on macOS");
      for (const std::string& attempted_loader_path : attempted_loader_paths) {
        REXLOG_ERROR("* tried {}", attempted_loader_path);
      }
      return nullptr;
    }
#else
    loader_loaded = vulkan_instance->loader_.Load(platform::lib_names::kVulkanLoader);
    if (!loader_loaded) {
      REXLOG_ERROR("Failed to load {}", platform::lib_names::kVulkanLoader);
      return nullptr;
    }
#endif

#define XE_VULKAN_LOAD_LOADER_FUNCTION(name)   functions_loaded &= (ifn.name = vulkan_instance->loader_.GetSymbol<PFN_##name>(#name)) != nullptr;
    XE_VULKAN_LOAD_LOADER_FUNCTION(vkGetInstanceProcAddr);
    XE_VULKAN_LOAD_LOADER_FUNCTION(vkDestroyInstance);
#undef XE_VULKAN_LOAD_LOADER_FUNCTION
    if (!functions_loaded) {
      REXLOG_ERROR("Failed to get Vulkan loader function pointers");
      return nullptr;
    }
  }

  // Load global functions.

  functions_loaded &= (ifn.vkCreateInstance = PFN_vkCreateInstance(
                           ifn.vkGetInstanceProcAddr(nullptr, "vkCreateInstance"))) != nullptr;
  functions_loaded &= (ifn.vkEnumerateInstanceExtensionProperties =
                           PFN_vkEnumerateInstanceExtensionProperties(ifn.vkGetInstanceProcAddr(
                               nullptr, "vkEnumerateInstanceExtensionProperties"))) != nullptr;
  functions_loaded &=
      (ifn.vkEnumerateInstanceLayerProperties = PFN_vkEnumerateInstanceLayerProperties(
           ifn.vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties"))) != nullptr;
  if (!functions_loaded) {
    REXLOG_ERROR(
        "Failed to get Vulkan global function pointers via "
        "vkGetInstanceProcAddr");
    return nullptr;
  }
  // Available since Vulkan 1.1. If this is nullptr, it's a Vulkan 1.0 instance.
  ifn.vkEnumerateInstanceVersion = PFN_vkEnumerateInstanceVersion(
      ifn.vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));

  // Get the API version.

  if (ifn.vkEnumerateInstanceVersion) {
    ifn.vkEnumerateInstanceVersion(&vulkan_instance->api_version_);
  }

  // Enable extensions and layers.

  // Name pointers from `requested_extensions` will be used in the enabled
  // extensions vector.
  std::unordered_map<std::string, bool*> requested_extensions;
  if (vulkan_instance->api_version_ >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    vulkan_instance->extensions_.ext_1_1_KHR_get_physical_device_properties2 = true;
  } else {
    // #60.
    requested_extensions.emplace(
        "VK_KHR_get_physical_device_properties2",
        &vulkan_instance->extensions_.ext_1_1_KHR_get_physical_device_properties2);
  }
  // #129.
  requested_extensions.emplace("VK_EXT_debug_utils",
                               &vulkan_instance->extensions_.ext_EXT_debug_utils);
  // #395.
  requested_extensions.emplace("VK_KHR_portability_enumeration",
                               &vulkan_instance->extensions_.ext_KHR_portability_enumeration);
  if (with_surface) {
    // #1.
    requested_extensions.emplace("VK_KHR_surface", &vulkan_instance->extensions_.ext_KHR_surface);
#ifdef VK_USE_PLATFORM_XCB_KHR
    // #6.
    requested_extensions.emplace("VK_KHR_xcb_surface",
                                 &vulkan_instance->extensions_.ext_KHR_xcb_surface);
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
    // #7.
    requested_extensions.emplace("VK_KHR_wayland_surface",
                                 &vulkan_instance->extensions_.ext_KHR_wayland_surface);
#endif
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    // #9.
    requested_extensions.emplace("VK_KHR_android_surface",
                                 &vulkan_instance->extensions_.ext_KHR_android_surface);
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
    // #10.
    requested_extensions.emplace("VK_KHR_win32_surface",
                                 &vulkan_instance->extensions_.ext_KHR_win32_surface);
#endif
#ifdef VK_USE_PLATFORM_METAL_EXT
    // #217.
    requested_extensions.emplace("VK_EXT_metal_surface",
                                 &vulkan_instance->extensions_.ext_EXT_metal_surface);
#endif
#ifdef VK_USE_PLATFORM_VI_NN
    // #63.
    requested_extensions.emplace("VK_NN_vi_surface",
                                 &vulkan_instance->extensions_.ext_NN_vi_surface);
#endif
  }

  std::vector<const char*> enabled_extensions;

  std::vector<VkExtensionProperties> supported_implementation_extensions;
  while (true) {
    uint32_t supported_implementation_extension_count = 0;
    const VkResult get_supported_implementation_extension_count_result =
        ifn.vkEnumerateInstanceExtensionProperties(
            nullptr, &supported_implementation_extension_count, nullptr);
    if (get_supported_implementation_extension_count_result != VK_SUCCESS &&
        get_supported_implementation_extension_count_result != VK_INCOMPLETE) {
      REXLOG_WARN("Failed to get the Vulkan instance extension count");
      return nullptr;
    }
    if (supported_implementation_extension_count) {
      supported_implementation_extensions.resize(supported_implementation_extension_count);
      const VkResult get_supported_implementation_extensions_result =
          ifn.vkEnumerateInstanceExtensionProperties(nullptr,
                                                     &supported_implementation_extension_count,
                                                     supported_implementation_extensions.data());
      if (get_supported_implementation_extensions_result == VK_INCOMPLETE) {
        continue;
      }
      if (get_supported_implementation_extensions_result != VK_SUCCESS) {
        REXLOG_WARN("Failed to get the Vulkan instance extensions");
        return nullptr;
      }
    }
    supported_implementation_extensions.resize(supported_implementation_extension_count);
    break;
  }

  for (const VkExtensionProperties& supported_extension : supported_implementation_extensions) {
    const auto requested_extension_it =
        requested_extensions.find(supported_extension.extensionName);
    if (requested_extension_it == requested_extensions.cend()) {
      continue;
    }
    assert_not_null(requested_extension_it->second);
    if (!*requested_extension_it->second) {
      enabled_extensions.emplace_back(requested_extension_it->first.c_str());
      *requested_extension_it->second = true;
    }
  }

  // If enabled layers are not present, will disable all extensions provided by
  // the layers by truncating the enabled extension vector to this size.
  const size_t enabled_implementation_extension_count = enabled_extensions.size();
  std::vector<bool*> enabled_layer_extension_enablement_bools;

  // Name pointers from `requested_layers` will be used in the enabled layer
  // vector.
  std::unordered_map<std::string, bool*> requested_layers;
  bool layer_khronos_validation = false;
  if (try_enable_validation) {
    requested_layers.emplace("VK_LAYER_KHRONOS_validation", &layer_khronos_validation);
  }

  std::vector<const char*> enabled_layers;

  if (!requested_layers.empty()) {
    std::vector<VkLayerProperties> available_layers;
    // "The list of available layers may change at any time due to actions
    // outside of the Vulkan implementation"
    while (true) {
      available_layers.clear();
      uint32_t available_layer_count = 0;
      const VkResult get_available_layer_count_result =
          ifn.vkEnumerateInstanceLayerProperties(&available_layer_count, nullptr);
      if (get_available_layer_count_result != VK_SUCCESS &&
          get_available_layer_count_result != VK_INCOMPLETE) {
        break;
      }
      if (available_layer_count) {
        available_layers.resize(available_layer_count);
        const VkResult get_available_layers_result =
            ifn.vkEnumerateInstanceLayerProperties(&available_layer_count, available_layers.data());
        if (get_available_layers_result == VK_INCOMPLETE) {
          // New layers were added.
          continue;
        }
        if (get_available_layers_result != VK_SUCCESS) {
          available_layers.clear();
          break;
        }
        // In case the second enumeration returned fewer layers.
        available_layers.resize(available_layer_count);
      }
      break;
    }

    if (!available_layers.empty()) {
      std::vector<VkExtensionProperties> supported_layer_extensions;

      for (const VkLayerProperties& available_layer : available_layers) {
        auto requested_layer_it = requested_layers.find(available_layer.layerName);
        if (requested_layer_it == requested_layers.cend()) {
          continue;
        }

        bool got_layer_extensions = true;
        // "Because the list of available layers may change externally between
        // calls to vkEnumerateInstanceExtensionProperties, two calls may
        // retrieve different results if a pLayerName is available in one call
        // but not in another."
        while (true) {
          uint32_t supported_layer_extension_count = 0;
          const VkResult get_supported_layer_extension_count_result =
              ifn.vkEnumerateInstanceExtensionProperties(nullptr, &supported_layer_extension_count,
                                                         nullptr);
          if (get_supported_layer_extension_count_result != VK_SUCCESS &&
              get_supported_layer_extension_count_result != VK_INCOMPLETE) {
            got_layer_extensions = false;
            break;
          }
          if (supported_layer_extension_count) {
            supported_layer_extensions.resize(supported_layer_extension_count);
            const VkResult get_supported_layer_extensions_result =
                ifn.vkEnumerateInstanceExtensionProperties(available_layer.layerName,
                                                           &supported_layer_extension_count,
                                                           supported_layer_extensions.data());
            if (get_supported_layer_extensions_result == VK_INCOMPLETE) {
              continue;
            }
            if (get_supported_layer_extensions_result != VK_SUCCESS) {
              got_layer_extensions = false;
              break;
            }
          }
          supported_layer_extensions.resize(supported_layer_extension_count);
          break;
        }
        if (!got_layer_extensions) {
          // The layer was possibly removed.
          continue;
        }

        for (const VkExtensionProperties& supported_extension : supported_layer_extensions) {
          const auto requested_extension_it =
              requested_extensions.find(supported_extension.extensionName);
          if (requested_extension_it == requested_extensions.cend()) {
            continue;
          }
          assert_not_null(requested_extension_it->second);
          // Don't add the extension to the enabled vector multiple times if
          // provided by the implementation itself or by another layer.
          if (!*requested_extension_it->second) {
            enabled_extensions.emplace_back(requested_extension_it->first.c_str());
            enabled_layer_extension_enablement_bools.push_back(requested_layer_it->second);
            *requested_extension_it->second = true;
          }
        }

        assert_not_null(requested_layer_it->second);
        if (!*requested_layer_it->second) {
          enabled_layers.emplace_back(requested_layer_it->first.c_str());
          *requested_layer_it->second = true;
        }
      }
    }
  }

  // Create the instance.

  VkApplicationInfo application_info;
  application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application_info.pNext = nullptr;
  application_info.pApplicationName = "Xenia";
  application_info.applicationVersion = 1;
  application_info.pEngineName = nullptr;
  application_info.engineVersion = 0;
  // "The patch version number specified in apiVersion is ignored when creating
  // an instance object."
  // "Vulkan 1.0 implementations were required to return
  // VK_ERROR_INCOMPATIBLE_DRIVER if apiVersion was larger than 1.0."
  application_info.apiVersion = vulkan_instance->api_version_ >= VK_MAKE_API_VERSION(0, 1, 1, 0)
                                    ? VulkanDevice::kHighestUsedApiMinorVersion
                                    : VK_MAKE_API_VERSION(0, 1, 0, 0);

  VkInstanceCreateInfo instance_create_info;
  instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_create_info.pNext = nullptr;
  instance_create_info.flags = 0;
  // VK_KHR_get_physical_device_properties2 is needed to get the portability
  // subset features.
  if (vulkan_instance->extensions_.ext_KHR_portability_enumeration &&
      vulkan_instance->extensions_.ext_1_1_KHR_get_physical_device_properties2) {
    instance_create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }
  instance_create_info.pApplicationInfo = &application_info;
  instance_create_info.enabledLayerCount = uint32_t(enabled_layers.size());
  instance_create_info.ppEnabledLayerNames = enabled_layers.data();
  instance_create_info.enabledExtensionCount = uint32_t(enabled_extensions.size());
  instance_create_info.ppEnabledExtensionNames = enabled_extensions.data();
  VkResult instance_create_result =
      ifn.vkCreateInstance(&instance_create_info, nullptr, &vulkan_instance->instance_);

  if (instance_create_result == VK_ERROR_LAYER_NOT_PRESENT ||
      instance_create_result == VK_ERROR_EXTENSION_NOT_PRESENT) {
    // A layer was possibly removed. Try without layers.
    for (bool* const extension_enablement : enabled_layer_extension_enablement_bools) {
      *extension_enablement = false;
    }
    for (const std::pair<std::string, bool*>& requested_layer : requested_layers) {
      *requested_layer.second = false;
    }
    instance_create_info.enabledLayerCount = 0;
    instance_create_info.enabledExtensionCount = uint32_t(enabled_implementation_extension_count);
    instance_create_result =
        ifn.vkCreateInstance(&instance_create_info, nullptr, &vulkan_instance->instance_);
  }

  if (instance_create_result != VK_SUCCESS) {
    REXLOG_ERROR("Failed to create a Vulkan instance: {}",
                 vk::to_string(vk::Result(instance_create_result)));
    return nullptr;
  }

  if (!ifn.vkDestroyInstance) {
    // Loader override path: not exported by a loader module, query the instance.
    ifn.vkDestroyInstance = PFN_vkDestroyInstance(
        ifn.vkGetInstanceProcAddr(vulkan_instance->instance_, "vkDestroyInstance"));
    if (!ifn.vkDestroyInstance) {
      REXLOG_ERROR("Failed to get vkDestroyInstance via the vkGetInstanceProcAddr override");
      return nullptr;  // Leaks the instance; the destructor cannot destroy it.
    }
  }

  // Load instance functions.

#define XE_UI_VULKAN_FUNCTION(name)                                                            \
  functions_loaded &=                                                                          \
      (ifn.name = PFN_##name(ifn.vkGetInstanceProcAddr(vulkan_instance->instance_, #name))) != \
      nullptr;

  // Vulkan 1.0.
#include <rex/ui/vulkan/functions/instance_1_0.inc>

  // Extensions promoted to a Vulkan version supported by the instance.
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name)                 \
  functions_loaded &= (ifn.core_name = PFN_##core_name(ifn.vkGetInstanceProcAddr( \
                           vulkan_instance->instance_, #core_name))) != nullptr;
  if (vulkan_instance->api_version_ >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
#include <rex/ui/vulkan/functions/instance_1_1_khr_get_physical_device_properties2.inc>
  }
#undef XE_UI_VULKAN_FUNCTION_PROMOTED

  // Non-promoted extensions, and extensions promoted to a Vulkan version not
  // supported by the instance.
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name)                 \
  functions_loaded &= (ifn.core_name = PFN_##core_name(ifn.vkGetInstanceProcAddr( \
                           vulkan_instance->instance_, #extension_name))) != nullptr;
  if (vulkan_instance->api_version_ < VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    if (vulkan_instance->extensions_.ext_1_1_KHR_get_physical_device_properties2) {
#include <rex/ui/vulkan/functions/instance_1_1_khr_get_physical_device_properties2.inc>
    }
  }
#ifdef VK_USE_PLATFORM_XCB_KHR
  if (vulkan_instance->extensions_.ext_KHR_xcb_surface) {
#include <rex/ui/vulkan/functions/instance_khr_xcb_surface.inc>
  }
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
  if (vulkan_instance->extensions_.ext_KHR_wayland_surface) {
#include <rex/ui/vulkan/functions/instance_khr_wayland_surface.inc>
  }
#endif
#ifdef VK_USE_PLATFORM_ANDROID_KHR
  if (vulkan_instance->extensions_.ext_KHR_android_surface) {
#include <rex/ui/vulkan/functions/instance_khr_android_surface.inc>
  }
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
  if (vulkan_instance->extensions_.ext_KHR_win32_surface) {
#include <rex/ui/vulkan/functions/instance_khr_win32_surface.inc>
  }
#endif
#ifdef VK_USE_PLATFORM_METAL_EXT
  if (vulkan_instance->extensions_.ext_EXT_metal_surface) {
#include <rex/ui/vulkan/functions/instance_ext_metal_surface.inc>
  }
#endif
#ifdef VK_USE_PLATFORM_VI_NN
  if (vulkan_instance->extensions_.ext_NN_vi_surface) {
#include <rex/ui/vulkan/functions/instance_nn_vi_surface.inc>
  }
#endif
  if (vulkan_instance->extensions_.ext_KHR_surface) {
#include <rex/ui/vulkan/functions/instance_khr_surface.inc>
  }
  if (vulkan_instance->extensions_.ext_EXT_debug_utils) {
#include <rex/ui/vulkan/functions/instance_ext_debug_utils.inc>
  }
#undef XE_UI_VULKAN_FUNCTION_PROMOTED

#undef XE_UI_VULKAN_FUNCTION

  if (!functions_loaded) {
    REXLOG_ERROR("Failed to get all Vulkan instance function pointers");
    return nullptr;
  }
  InstallGpuEmulation(ifn);

  // Check whether a surface can be created.

  if (with_surface &&
      !VulkanPresenter::GetSurfaceTypesSupportedByInstance(vulkan_instance->extensions_)) {
    REXLOG_ERROR("The Vulkan instance doesn't support surface types used by Xenia");
    return nullptr;
  }

  // Log instance properties.

  REXLOG_INFO("Vulkan instance API version {}.{}.{}. Enabled layers and extensions:",
              VK_VERSION_MAJOR(vulkan_instance->api_version_),
              VK_VERSION_MINOR(vulkan_instance->api_version_),
              VK_VERSION_PATCH(vulkan_instance->api_version_));
  for (uint32_t enabled_layer_index = 0;
       enabled_layer_index < instance_create_info.enabledLayerCount; ++enabled_layer_index) {
    REXLOG_INFO("* {}", instance_create_info.ppEnabledLayerNames[enabled_layer_index]);
  }
  for (uint32_t enabled_extension_index = 0;
       enabled_extension_index < instance_create_info.enabledExtensionCount;
       ++enabled_extension_index) {
    REXLOG_INFO("* {}", instance_create_info.ppEnabledExtensionNames[enabled_extension_index]);
  }

  // Create the debug messenger if requested and available.

  if (vulkan_instance->extensions_.ext_EXT_debug_utils && REXCVAR_GET(vulkan_log_debug_messages)) {
    VkDebugUtilsMessengerCreateInfoEXT debug_utils_messenger_create_info = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    auto gpu_logger = rex::GetLogger(rex::log::gpu());
    if (gpu_logger) {
      if (gpu_logger->should_log(spdlog::level::debug)) {
        debug_utils_messenger_create_info.messageSeverity |=
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
      }
      if (gpu_logger->should_log(spdlog::level::info)) {
        debug_utils_messenger_create_info.messageSeverity |=
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
      }
      if (gpu_logger->should_log(spdlog::level::warn)) {
        debug_utils_messenger_create_info.messageSeverity |=
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
      }
      if (gpu_logger->should_log(spdlog::level::err)) {
        debug_utils_messenger_create_info.messageSeverity |=
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      }
    }
    // VUID-VkDebugUtilsMessengerCreateInfoEXT-messageSeverity-requiredbitmask:
    // "messageSeverity must not be 0"
    if (debug_utils_messenger_create_info.messageSeverity) {
      debug_utils_messenger_create_info.messageType =
          VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      debug_utils_messenger_create_info.pfnUserCallback = DebugUtilsMessengerCallback;
      debug_utils_messenger_create_info.pUserData = vulkan_instance.get();
      const VkResult debug_utils_messenger_create_result = ifn.vkCreateDebugUtilsMessengerEXT(
          vulkan_instance->instance_, &debug_utils_messenger_create_info, nullptr,
          &vulkan_instance->debug_utils_messenger_);
      if (debug_utils_messenger_create_result != VK_SUCCESS) {
        REXLOG_WARN("Failed to create the Vulkan debug utils messenger: {}",
                    vk::to_string(vk::Result(debug_utils_messenger_create_result)));
      }
    }
  }

  return vulkan_instance;
}

VulkanInstance::~VulkanInstance() {
  if (instance_) {
    if (debug_utils_messenger_ != VK_NULL_HANDLE) {
      functions_.vkDestroyDebugUtilsMessengerEXT(instance_, debug_utils_messenger_, nullptr);
    }

    if (functions_.vkDestroyInstance) {
      functions_.vkDestroyInstance(instance_, nullptr);
    }
  }

  loader_.Close();
}

void VulkanInstance::EnumeratePhysicalDevices(
    std::vector<VkPhysicalDevice>& physical_devices_out) const {
  physical_devices_out.clear();
  while (true) {
    uint32_t physical_device_count = 0;
    const VkResult get_physical_device_count_result =
        functions_.vkEnumeratePhysicalDevices(instance_, &physical_device_count, nullptr);
    if ((get_physical_device_count_result != VK_SUCCESS &&
         get_physical_device_count_result != VK_INCOMPLETE) ||
        !physical_device_count) {
      return;
    }
    physical_devices_out.resize(physical_device_count);
    const VkResult get_physical_devices_result = functions_.vkEnumeratePhysicalDevices(
        instance_, &physical_device_count, physical_devices_out.data());
    if (get_physical_devices_result == VK_INCOMPLETE) {
      continue;
    }
    physical_devices_out.resize(get_physical_devices_result == VK_SUCCESS ? physical_device_count
                                                                          : 0);
    return;
  }
}

VkBool32 VulkanInstance::DebugUtilsMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
    VkDebugUtilsMessageTypeFlagsEXT message_types,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data, [[maybe_unused]] void* user_data) {
  std::ostringstream log_str;

  log_str << "Vulkan " << vk::to_string(vk::DebugUtilsMessageSeverityFlagBitsEXT(message_severity))
          << " (" << vk::to_string(vk::DebugUtilsMessageTypeFlagsEXT(message_types)) << ", ID "
          << callback_data->messageIdNumber;
  if (callback_data->pMessageIdName) {
    log_str << ": " << callback_data->pMessageIdName;
  }
  log_str << ')';

  if (callback_data->pMessage) {
    log_str << ": " << callback_data->pMessage;
  }

  bool annotations_begun = false;
  const auto begin_annotation = [&log_str, &annotations_begun]() {
    log_str << (annotations_begun ? ", " : " (");
    annotations_begun = true;
  };

  for (uint32_t queue_label_index = 0; queue_label_index < callback_data->queueLabelCount;
       ++queue_label_index) {
    begin_annotation();
    log_str << "queue label " << queue_label_index << ": "
            << callback_data->pQueueLabels[queue_label_index].pLabelName;
  }

  for (uint32_t cmd_buf_label_index = 0; cmd_buf_label_index < callback_data->cmdBufLabelCount;
       ++cmd_buf_label_index) {
    begin_annotation();
    log_str << "command buffer label " << cmd_buf_label_index << ": "
            << callback_data->pCmdBufLabels[cmd_buf_label_index].pLabelName;
  }

  for (uint32_t object_index = 0; object_index < callback_data->objectCount; ++object_index) {
    begin_annotation();
    const VkDebugUtilsObjectNameInfoEXT& object_info = callback_data->pObjects[object_index];
    // Lowercase hexadecimal digits in the handle to match the default Vulkan
    // debug utils messenger.
    log_str << "object " << object_index << ": "
            << vk::to_string(vk::ObjectType(object_info.objectType)) << " 0x" << std::hex
            << object_info.objectHandle << std::dec;
    if (object_info.pObjectName) {
      log_str << " '" << object_info.pObjectName << '\'';
    }
  }

  if (annotations_begun) {
    log_str << ')';
  }

  auto msg = log_str.str();
  if (message_severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    REXGPU_ERROR("{}", msg);
  } else if (message_severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    REXGPU_WARN("{}", msg);
  } else if (message_severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
    REXGPU_INFO("{}", msg);
  } else {
    REXGPU_DEBUG("{}", msg);
  }

  return VK_FALSE;
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
