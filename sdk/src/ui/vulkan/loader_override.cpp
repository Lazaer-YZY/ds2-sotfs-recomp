/**
 * @file        rex/ui/vulkan/loader_override.cpp
 * @brief       Storage for the process-wide vkGetInstanceProcAddr override
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>

#include <rex/ui/vulkan/loader_override.h>

namespace rex::ui::vulkan {

namespace {
std::atomic<void*> instance_proc_addr_override{nullptr};
}  // namespace

void SetInstanceProcAddrOverride(void* pfn_vkGetInstanceProcAddr) {
  instance_proc_addr_override.store(pfn_vkGetInstanceProcAddr, std::memory_order_release);
}

void* GetInstanceProcAddrOverride() {
  return instance_proc_addr_override.load(std::memory_order_acquire);
}

}  // namespace rex::ui::vulkan
