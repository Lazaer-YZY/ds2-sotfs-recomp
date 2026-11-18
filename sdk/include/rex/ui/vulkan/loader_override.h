#pragma once
/**
 * @file        rex/ui/vulkan/loader_override.h
 * @brief       Process-wide override of the Vulkan loader entry point
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

namespace rex::ui::vulkan {

// Hands the runtime a custom vkGetInstanceProcAddr (e.g. a Turnip driver
// loaded through libadrenotools on Android). Call before the GPU plugin
// creates its VulkanInstance; once set, VulkanInstance::Create does not load
// the system Vulkan loader and resolves everything through this function.
// Pass nullptr to clear. Plain void* so callers need no Vulkan headers.
//
// The storage lives once in librexruntime (rexui objects are only linked into
// rexruntime; see cmake/rexglue_link_audit.cmake), so the game library and
// the GPU plugin observe the same value through these exported accessors.
void SetInstanceProcAddrOverride(void* pfn_vkGetInstanceProcAddr);
void* GetInstanceProcAddrOverride();

}  // namespace rex::ui::vulkan
