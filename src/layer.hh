#ifndef LAYER_HH_
#define LAYER_HH_

#include <vulkan/vk_platform.h>
#include <vulkan/vulkan.hpp>

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
LowLatency_GetInstanceProcAddr(VkInstance instance, const char* const pname);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
LowLatency_GetDeviceProcAddr(VkDevice device, const char* pName);
}

namespace low_latency {

static constexpr auto LAYER_NAME = "VK_LAYER_NJ3AHXAC_LowLatency";

}

#endif