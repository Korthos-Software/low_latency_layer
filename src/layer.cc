#include "layer.hh"

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include <cstring>
#include <iostream>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace low_latency {

static auto mutex = std::mutex{};

struct command_stats {
    std::uint32_t num_draws;
    std::uint32_t num_instances;
    std::uint32_t num_verts;
};
static std::unordered_map<VkCommandBuffer, command_stats>
    commandbuffer_to_stats{};
static std::unordered_map<void*, VkuInstanceDispatchTable> instance_dispatch;
static std::unordered_map<void*, VkuDeviceDispatchTable> device_dispatch;

template <typename T>
concept DispatchableType =
    std::same_as<std::remove_cvref_t<T>, VkQueue> ||
    std::same_as<std::remove_cvref_t<T>, VkCommandBuffer> ||
    std::same_as<std::remove_cvref_t<T>, VkInstance> ||
    std::same_as<std::remove_cvref_t<T>, VkDevice> ||
    std::same_as<std::remove_cvref_t<T>, VkPhysicalDevice>;
template <DispatchableType T> void* get_key(const T& inst) {
    return *reinterpret_cast<void**>(inst);
}

static VKAPI_ATTR VkResult VKAPI_CALL
BeginCommandBuffer(VkCommandBuffer command_buffer,
                   const VkCommandBufferBeginInfo* begin_info) {
    const auto lock = std::scoped_lock{mutex};
    commandbuffer_to_stats[command_buffer] = {};
    return device_dispatch[get_key(command_buffer)].BeginCommandBuffer(
        command_buffer, begin_info);
}

static VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer command_buffer,
                                          std::uint32_t vertex_count,
                                          std::uint32_t instance_count,
                                          std::uint32_t first_vertex,
                                          std::uint32_t first_instance) {

    const auto lock = std::scoped_lock{mutex};

    if (const auto it = commandbuffer_to_stats.find(command_buffer);
        it != std::end(commandbuffer_to_stats)) {

        auto& stats = it->second;
        stats.num_draws++;
        stats.num_instances += instance_count;
        stats.num_verts += instance_count * vertex_count;
    }

    device_dispatch[get_key(command_buffer)].CmdDraw(
        command_buffer, vertex_count, instance_count, first_vertex,
        first_instance);
}

static VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer command_buffer,
                                                 uint32_t index_count,
                                                 uint32_t instance_count,
                                                 uint32_t first_index,
                                                 int32_t vertex_offset,
                                                 uint32_t first_instance) {

    const auto lock = std::scoped_lock{mutex};

    if (const auto it = commandbuffer_to_stats.find(command_buffer);
        it != std::end(commandbuffer_to_stats)) {

        auto& stats = it->second;
        stats.num_draws++;
        stats.num_instances += instance_count;
        stats.num_verts += instance_count * index_count;
    }

    device_dispatch[get_key(command_buffer)].CmdDrawIndexed(
        command_buffer, index_count, instance_count, first_index, vertex_offset,
        first_instance);
}

static VKAPI_ATTR VkResult VKAPI_CALL
EndCommandBuffer(VkCommandBuffer command_buffer) {

    const auto lock = std::scoped_lock{mutex};

    const auto& s = commandbuffer_to_stats[command_buffer];

    std::cout << std::format("Command buffer ended with {} draws, {} "
                             "instances and {} vertices\n",
                             s.num_draws, s.num_instances, s.num_verts);

    const auto it = device_dispatch.find(get_key(command_buffer));
    if (it == std::end(device_dispatch)) {
        return VK_ERROR_DEVICE_LOST;
    }
    return it->second.EndCommandBuffer(command_buffer);
}

static VKAPI_ATTR VkResult VKAPI_CALL
CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
               const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {

    // Iterate through list starting at pNext until we see create_info and
    // link_info.
    auto layer_create_info = [&]() -> VkLayerInstanceCreateInfo* {
        for (auto base =
                 reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             base; base = base->pNext) {

            if (base->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
                continue;
            }

            const auto info =
                reinterpret_cast<const VkLayerInstanceCreateInfo*>(base);
            if (info->function != VK_LAYER_LINK_INFO) {
                continue;
            }
            return const_cast<VkLayerInstanceCreateInfo*>(info);
        }
        return nullptr;
    }();

    if (!layer_create_info || !layer_create_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Store our get instance proc addr function and pop it off our list +
    // advance the list so future layers know what to call.
    const auto next_gipa =
        layer_create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    if (!next_gipa) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    layer_create_info->u.pLayerInfo = layer_create_info->u.pLayerInfo->pNext;

    // Call our create instance func, and store vkDestroyInstance, and
    // vkCreateDevice as well.
    const auto create_instance_func = reinterpret_cast<PFN_vkCreateInstance>(
        next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create_instance_func) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (const auto result =
            create_instance_func(pCreateInfo, pAllocator, pInstance);
        result != VK_SUCCESS) {

        return result;
    }

    const auto lock = std::scoped_lock{mutex};
    instance_dispatch.emplace(
        get_key(*pInstance),
        VkuInstanceDispatchTable{
            .DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
                next_gipa(*pInstance, "vkDestroyInstance")),
            .GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                next_gipa(*pInstance, "vkGetInstanceProcAddr")),
            .EnumerateDeviceExtensionProperties =
                reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                    next_gipa(*pInstance,
                              "vkEnumerateDeviceExtensionProperties")),
        }

    );

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {

    const auto lock = std::scoped_lock{mutex};
    instance_dispatch.erase(get_key(instance));
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {

    auto layer_create_info = [&]() -> VkLayerDeviceCreateInfo* {
        for (auto base =
                 reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             base; base = base->pNext) {

            if (base->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
                continue;
            }

            const auto info =
                reinterpret_cast<const VkLayerDeviceCreateInfo*>(base);

            if (info->function != VK_LAYER_LINK_INFO) {
                continue;
            }

            return const_cast<VkLayerDeviceCreateInfo*>(info);
        }
        return nullptr;
    }();

    if (!layer_create_info || !layer_create_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const auto next_gipa =
        layer_create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto next_gdpa =
        layer_create_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    if (!next_gipa || !next_gdpa) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    layer_create_info->u.pLayerInfo = layer_create_info->u.pLayerInfo->pNext;

    const auto create_func = reinterpret_cast<PFN_vkCreateDevice>(
        next_gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!create_func) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (const auto result =
            create_func(physical_device, pCreateInfo, pAllocator, pDevice);
        result != VK_SUCCESS) {
        return result;
    }

    const auto lock = std::scoped_lock{mutex};
    device_dispatch.emplace(
        get_key(*pDevice),
        VkuDeviceDispatchTable{
            .GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                next_gdpa(*pDevice, "vkGetDeviceProcAddr")),
            .DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
                next_gdpa(*pDevice, "vkDestroyDevice")),
            .BeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(
                next_gdpa(*pDevice, "vkBeginCommandBuffer")),
            .EndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(
                next_gdpa(*pDevice, "vkEndCommandBuffer")),
            .CmdDraw = reinterpret_cast<PFN_vkCmdDraw>(
                next_gdpa(*pDevice, "vkCmdDraw")),
            .CmdDrawIndexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(
                next_gdpa(*pDevice, "vkCmdDrawIndexed")),
        });

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {

    const auto lock = std::scoped_lock{mutex};
    device_dispatch.erase(get_key(device));
}

// These are wrong, the tutorial isn't correct afaik.
static VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(
    std::uint32_t* pPropertyCount, VkLayerProperties* pProperties) {

    if (pPropertyCount) {
        *pPropertyCount = 1;
    }

    if (pProperties) {
        std::strcpy(pProperties->layerName, LAYER_NAME);
        std::strcpy(pProperties->description, "Low Latency Layer");
        pProperties->implementationVersion = 1;
        pProperties->specVersion = VK_API_VERSION_1_3;
    }

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(
    VkPhysicalDevice physical_device, uint32_t* pPropertyCount,
    VkLayerProperties* pProperties) {

    return EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(
    const char* pLayerName, uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {

    if (!pLayerName || std::string_view{pLayerName} != LAYER_NAME) {

        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    if (pPropertyCount) {
        *pPropertyCount = 0;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device, const char* pLayerName,
    uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {

    if (!pLayerName || std::string_view{pLayerName} != LAYER_NAME) {

        if (physical_device == VK_NULL_HANDLE) {
            return VK_SUCCESS;
        }

        const auto lock = std::scoped_lock{mutex};
        return instance_dispatch[get_key(physical_device)]
            .EnumerateDeviceExtensionProperties(physical_device, pLayerName,
                                                pPropertyCount, pProperties);
    }

    if (pPropertyCount) {
        *pPropertyCount = 0;
    }
    return VK_SUCCESS;
}

} // namespace low_latency

static const auto instance_functions =
    std::unordered_map<std::string_view, const PFN_vkVoidFunction>{
        {"vkGetInstanceProcAddr",
         reinterpret_cast<PFN_vkVoidFunction>(LowLatency_GetInstanceProcAddr)},

        {"vkEnumerateInstanceLayerProperties",
         reinterpret_cast<PFN_vkVoidFunction>(
             low_latency::EnumerateInstanceLayerProperties)},
        {"vkEnumerateInstanceExtensionProperties",
         reinterpret_cast<PFN_vkVoidFunction>(
             low_latency::EnumerateInstanceExtensionProperties)},

        {"vkCreateInstance",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::CreateInstance)},
        {"vkDestroyInstance",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::DestroyInstance)},
    };

static const auto device_functions =
    std::unordered_map<std::string_view, const PFN_vkVoidFunction>{
        {"vkGetDeviceProcAddr",
         reinterpret_cast<PFN_vkVoidFunction>(LowLatency_GetDeviceProcAddr)},

        {"vkEnumerateDeviceLayerProperties",
         reinterpret_cast<PFN_vkVoidFunction>(
             low_latency::EnumerateDeviceLayerProperties)},
        {"vkEnumerateDeviceExtensionProperties",
         reinterpret_cast<PFN_vkVoidFunction>(
             low_latency::EnumerateDeviceExtensionProperties)},

        {"vkCreateDevice",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::CreateDevice)},
        {"vkDestroyDevice",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::DestroyDevice)},

        {"vkCmdDraw",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::CmdDraw)},
        {"vkCmdDrawIndexed",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::CmdDrawIndexed)},

        {"vkBeginCommandBuffer",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::BeginCommandBuffer)},
        {"vkEndCommandBuffer",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::EndCommandBuffer)},
    };

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
LowLatency_GetDeviceProcAddr(VkDevice device, const char* const pName) {

    if (const auto it = device_functions.find(pName);
        it != std::end(device_functions)) {

        return it->second;
    }

    const auto lock = std::scoped_lock{low_latency::mutex};
    return low_latency::device_dispatch[low_latency::get_key(device)]
        .GetDeviceProcAddr(device, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
LowLatency_GetInstanceProcAddr(VkInstance instance, const char* const pName) {

    for (const auto& functions : {device_functions, instance_functions}) {
        const auto it = functions.find(pName);
        if (it == std::end(functions)) {
            continue;
        }
        return it->second;
    }

    const auto lock = std::scoped_lock{low_latency::mutex};
    return low_latency::instance_dispatch[low_latency::get_key(instance)]
        .GetInstanceProcAddr(instance, pName);
}