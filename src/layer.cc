#include "layer.hh"

#include <iostream>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include "device_context.hh"
#include "instance_context.hh"
#include "layer_context.hh"
#include "queue_context.hh"

namespace low_latency {

namespace {

LayerContext layer_context;

} // namespace

template <typename T, typename sType>
static T* get_link_info(const void* const head, const sType& stype) {
    for (auto i = reinterpret_cast<const VkBaseInStructure*>(head); i;
         i = i->pNext) {

        if (i->sType != stype) {
            continue;
        }

        const auto info = reinterpret_cast<const T*>(i);
        if (info->function != VK_LAYER_LINK_INFO) {
            continue;
        }

        return const_cast<T*>(info);
    }
    return nullptr;
}

static VKAPI_ATTR VkResult VKAPI_CALL
CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
               const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {

    const auto link_info = get_link_info<VkLayerInstanceCreateInfo>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);

    if (!link_info || !link_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Store our get instance proc addr function and pop it off our list +
    // advance the list so future layers know what to call.
    const auto gipa = link_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    if (!gipa) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    link_info->u.pLayerInfo = link_info->u.pLayerInfo->pNext;

    // Call our create instance func, and store vkDestroyInstance, and
    // vkCreateDevice as well.
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
        gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create_instance) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (const auto result = create_instance(pCreateInfo, pAllocator, pInstance);
        result != VK_SUCCESS) {

        return result;
    }

    const auto key = layer_context.get_key(*pInstance);
    auto vtable = VkuInstanceDispatchTable{
        .DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
            gipa(*pInstance, "vkDestroyInstance")),
        .EnumeratePhysicalDevices =
            reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
                gipa(*pInstance, "vkEnumeratePhysicalDevices")),
        .GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            gipa(*pInstance, "vkGetInstanceProcAddr")),
        .EnumerateDeviceExtensionProperties =
            reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                gipa(*pInstance, "vkEnumerateDeviceExtensionProperties")),
    };

    const auto lock = std::scoped_lock{layer_context.mutex};
    assert(!layer_context.contexts.contains(key));
    layer_context.contexts.try_emplace(
        key, std::make_unique<InstanceContext>(*pInstance, std::move(vtable)));

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {

    const auto lock = std::scoped_lock{layer_context.mutex};

    const auto key = layer_context.get_key(instance);
    assert(layer_context.contexts.contains(key));
    layer_context.contexts.erase(key);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {

    const auto create_info = get_link_info<VkLayerDeviceCreateInfo>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!create_info || !create_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const auto gipa = create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gdpa = create_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    if (!gipa || !gdpa) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    create_info->u.pLayerInfo = create_info->u.pLayerInfo->pNext;

    const auto lock = std::scoped_lock{layer_context.mutex};

    auto& context = layer_context.get_context<InstanceContext>(physical_device);

    const auto next_extensions =
        [&]() -> std::optional<std::vector<const char*>> {
        const auto supported_extensions =
            [&]() -> std::optional<std::vector<VkExtensionProperties>> {
            const auto enumerate_device_extensions =
                reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(
                    context.instance, "vkEnumerateDeviceExtensionProperties"));
            if (!enumerate_device_extensions) {
                return std::nullopt;
            }

            auto count = std::uint32_t{};
            if (enumerate_device_extensions(physical_device, nullptr, &count,
                                            nullptr) != VK_SUCCESS) {

                return std::nullopt;
            }

            auto supported_extensions =
                std::vector<VkExtensionProperties>(count);
            if (enumerate_device_extensions(physical_device, nullptr, &count,
                                            std::data(supported_extensions)) !=
                VK_SUCCESS) {

                return std::nullopt;
            }

            return supported_extensions;
        }();

        auto next_extensions =
            std::vector{*pCreateInfo->ppEnabledExtensionNames,
                        std::next(*pCreateInfo->ppEnabledExtensionNames +
                                  pCreateInfo->enabledExtensionCount)};

        const auto wanted_extensions = {
            VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
            VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
            VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};

        for (const auto& wanted : wanted_extensions) {

            if (std::ranges::any_of(
                    next_extensions, [&](const auto& next_extension) {
                        return !std::strcmp(next_extension, wanted);
                    })) {

                continue; // Already included, ignore it.
            }

            if (std::ranges::none_of(*supported_extensions,
                                     [&](const auto& supported_extension) {
                                         return !std::strcmp(
                                             supported_extension.extensionName,
                                             wanted);
                                     })) {

                return std::nullopt; // We don't support it, the layer can't
                                     // work.
            }

            next_extensions.push_back(wanted);
        }

        return next_extensions;
    }();

    if (!next_extensions.has_value()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const auto create_device = reinterpret_cast<PFN_vkCreateDevice>(
        gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!create_device) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const auto next_create_info = [&]() -> VkDeviceCreateInfo {
        auto next_pCreateInfo = *pCreateInfo;
        next_pCreateInfo.ppEnabledExtensionNames = std::data(*next_extensions);
        next_pCreateInfo.enabledExtensionCount = std::size(*next_extensions);
        return next_pCreateInfo;
    }();

    if (const auto result = create_device(physical_device, &next_create_info,
                                          pAllocator, pDevice);
        result != VK_SUCCESS) {

        return result;
    }

    auto vtable = VkuDeviceDispatchTable{
        .GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            gdpa(*pDevice, "vkGetDeviceProcAddr")),
        .DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
            gdpa(*pDevice, "vkDestroyDevice")),
        .GetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
            gdpa(*pDevice, "vkGetDeviceQueue")),
        .QueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
            gdpa(*pDevice, "vkQueueSubmit")),
        .CreateSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(
            gdpa(*pDevice, "vkCreateSemaphore")),
        .DestroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(
            gdpa(*pDevice, "vkDestroySemaphore")),
        .CreateQueryPool = reinterpret_cast<PFN_vkCreateQueryPool>(
            gdpa(*pDevice, "vkCreateQueryPool")),
        .DestroyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool>(
            gdpa(*pDevice, "vkDestroyQueryPool")),
        .GetQueryPoolResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(
            gdpa(*pDevice, "vkGetQueryPoolResults")),
        .CreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(
            gdpa(*pDevice, "vkCreateCommandPool")),
        .DestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
            gdpa(*pDevice, "vkDestroyCommandPool")),
        .AllocateCommandBuffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(
                gdpa(*pDevice, "vkAllocateCommandBuffers")),
        .FreeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(
            gdpa(*pDevice, "vkFreeCommandBuffers")),
        .BeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(
            gdpa(*pDevice, "vkBeginCommandBuffer")),
        .EndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(
            gdpa(*pDevice, "vkEndCommandBuffer")),
        .ResetCommandBuffer = reinterpret_cast<PFN_vkResetCommandBuffer>(
            gdpa(*pDevice, "vkResetCommandBuffer")),
        .CmdDraw = reinterpret_cast<PFN_vkCmdDraw>(gdpa(*pDevice, "vkCmdDraw")),
        .CmdDrawIndexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(
            gdpa(*pDevice, "vkCmdDrawIndexed")),
        .CmdResetQueryPool = reinterpret_cast<PFN_vkCmdResetQueryPool>(
            gdpa(*pDevice, "vkCmdResetQueryPool")),
        .GetDeviceQueue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(
            gdpa(*pDevice, "vkGetDeviceQueue2")),
        .QueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(
            gdpa(*pDevice, "vkQueueSubmit2")),
        .QueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
            gdpa(*pDevice, "vkQueuePresentKHR")),
        .GetSemaphoreCounterValueKHR =
            reinterpret_cast<PFN_vkGetSemaphoreCounterValueKHR>(
                gdpa(*pDevice, "vkGetSemaphoreCounterValueKHR")),
        .CmdWriteTimestamp2KHR = reinterpret_cast<PFN_vkCmdWriteTimestamp2KHR>(
            gdpa(*pDevice, "vkCmdWriteTimestamp2KHR")),
        .QueueSubmit2KHR = reinterpret_cast<PFN_vkQueueSubmit2KHR>(
            gdpa(*pDevice, "vkQueueSubmit2KHR")),
    };

    const auto key = layer_context.get_key(*pDevice);
    assert(!layer_context.contexts.contains(key));
    layer_context.contexts.try_emplace(
        key,
        std::make_unique<DeviceContext>(context, *pDevice, std::move(vtable)));

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    const auto lock = std::scoped_lock{layer_context.mutex};
    const auto key = layer_context.get_key(device);
    assert(layer_context.contexts.contains(key));
    layer_context.contexts.erase(key);
}

// Small amount of duplication, we can't assume gdq2 is available apparently.
static VKAPI_ATTR void VKAPI_CALL
GetDeviceQueue(VkDevice device, std::uint32_t queue_family_index,
               std::uint32_t queue_index, VkQueue* queue) {

    const auto lock = std::scoped_lock{layer_context.mutex};

    auto& device_context = layer_context.get_context<DeviceContext>(device);

    device_context.vtable.GetDeviceQueue(device, queue_family_index,
                                         queue_index, queue);
    if (!queue || !*queue) {
        return;
    }

    auto& queue_contexts = device_context.queue_contexts;
    if (!queue_contexts.contains(*queue)) {
        queue_contexts.try_emplace(
            *queue, std::make_unique<QueueContext>(device_context, *queue,
                                                   queue_family_index));
    }
}

static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {

    const auto lock = std::scoped_lock{layer_context.mutex};
    auto& device_context = layer_context.get_context<DeviceContext>(device);

    device_context.vtable.GetDeviceQueue2(device, info, queue);
    if (!queue || !*queue) {
        return;
    }

    auto& queue_contexts = device_context.queue_contexts;
    if (!queue_contexts.contains(*queue)) {
        queue_contexts.try_emplace(
            *queue, std::make_unique<QueueContext>(device_context, *queue,
                                                   info->queueFamilyIndex));
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(VkQueue queue, std::uint32_t submit_count,
              const VkSubmitInfo* submit_info, VkFence fence) {

    const auto lock = std::scoped_lock{layer_context.mutex};

    auto& queue_context = layer_context.get_context<QueueContext>(queue);
    const auto& vtable = queue_context.device_context.vtable;

    if (!submit_count) { // no-op submit we shouldn't worry about
        return vtable.QueueSubmit(queue, submit_count, submit_info, fence);
    }

    // Create a new vector of submit infos, copy their existing ones.
    auto next_submit_infos = std::vector<VkSubmitInfo>{};
    next_submit_infos.reserve(submit_count + 2);

    auto timestamp_handle = queue_context.timestamp_pool->acquire();
    timestamp_handle->setup_command_buffers(vtable);

    const auto& [head_cb, tail_cb] = timestamp_handle->command_buffers;

    // The first submit info we use will steal their wait semaphores.
    next_submit_infos.push_back(VkSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = submit_info->pNext,
        .waitSemaphoreCount = submit_info[0].waitSemaphoreCount,
        .pWaitSemaphores = submit_info[0].pWaitSemaphores,
        .pWaitDstStageMask = submit_info[0].pWaitDstStageMask,
        .commandBufferCount = 1,
        .pCommandBuffers = &head_cb,
    });

    // Fill in original submit infos but erase the wait semaphores on the
    // first because we stole them earlier.
    std::ranges::copy_n(submit_info, submit_count,
                        std::back_inserter(next_submit_infos));
    next_submit_infos[1].pWaitSemaphores = nullptr;
    next_submit_infos[1].waitSemaphoreCount = 0u;

    const auto TODO_next = std::uint64_t{layer_context.current_frame + 1};
    const auto tail_tssi = VkTimelineSemaphoreSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &TODO_next,
    };
    next_submit_infos.push_back(VkSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &tail_tssi,
        .commandBufferCount = 1,
        .pCommandBuffers = &tail_cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &queue_context.semaphore,
    });

    if (const auto res =
            vtable.QueueSubmit(queue, std::size(next_submit_infos),
                               std::data(next_submit_infos), fence);
        res != VK_SUCCESS) {

        return res;
    }

    return VK_SUCCESS;
}

// The logic for this function is identical to vkSubmitInfo.
static VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit2(VkQueue queue, std::uint32_t submit_count,
               const VkSubmitInfo2* submit_infos, VkFence fence) {

    const auto lock = std::scoped_lock{layer_context.mutex};
    auto& queue_context = layer_context.get_context<QueueContext>(queue);
    const auto& vtable = queue_context.device_context.vtable;

    if (!submit_count) { // another no-op submit
        return vtable.QueueSubmit2(queue, submit_count, submit_infos, fence);
    }

    auto next_submit_infos = std::vector<VkSubmitInfo2>();
    next_submit_infos.reserve(submit_count + 2);

    auto timestamp_handle = queue_context.timestamp_pool->acquire();
    timestamp_handle->setup_command_buffers(vtable);
    const auto& [head_cb, tail_cb] = timestamp_handle->command_buffers;

    const auto head_cb_info = VkCommandBufferSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = head_cb,
    };
    next_submit_infos.push_back(VkSubmitInfo2{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = submit_infos[0].waitSemaphoreInfoCount,
        .pWaitSemaphoreInfos = submit_infos[0].pWaitSemaphoreInfos,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &head_cb_info,
    });
    std::ranges::copy_n(submit_infos, submit_count,
                        std::back_inserter(next_submit_infos));
    next_submit_infos[1].pWaitSemaphoreInfos = nullptr;
    next_submit_infos[1].waitSemaphoreInfoCount = 0;

    const auto tail_cb_info = VkCommandBufferSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = tail_cb,
    };
    next_submit_infos.push_back(VkSubmitInfo2{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = submit_infos[0].waitSemaphoreInfoCount,
        .pWaitSemaphoreInfos = submit_infos[0].pWaitSemaphoreInfos,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &tail_cb_info,
    });

    if (const auto res =
            vtable.QueueSubmit2(queue, submit_count, submit_infos, fence);
        res != VK_SUCCESS) {
        return res;
    }

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit2KHR(VkQueue queue, std::uint32_t submit_count,
                  const VkSubmitInfo2* submit_info, VkFence fence) {
    // Just forward to low_latency::vkQueueSubmit2 here.
    return low_latency::vkQueueSubmit2(queue, submit_count, submit_info, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* present_info) {

    const auto lock = std::scoped_lock{layer_context.mutex};
    auto& queue_context = layer_context.get_context<QueueContext>(queue);
    const auto& vtable = queue_context.device_context.vtable;

    if (const auto res = vtable.QueuePresentKHR(queue, present_info);
        res != VK_SUCCESS) {

        return res;
    }

    std::cout << "queuePresentKHR called for queue " << queue << '\n';

    // Update all of our information about this queue's timestamp pool!
    queue_context.timestamp_pool->poll();

    // While we might be submitting on this queue, let's see what our timeline
    // semaphore says we're at.
    uint64_t value = 0;
    if (const auto res = vtable.GetSemaphoreCounterValueKHR(
            queue_context.device_context.device, queue_context.semaphore,
            &value);
        res != VK_SUCCESS) {

        return res;
    }

    std::cout << "    frame_index: " << layer_context.current_frame << '\n';
    std::cout << "    semaphore: " << value << '\n';
    std::cout << "    queue: " << queue << '\n';

    ++layer_context.current_frame;
    return VK_SUCCESS;
}

} // namespace low_latency

static const auto instance_functions =
    std::unordered_map<std::string_view, const PFN_vkVoidFunction>{
        {"vkGetInstanceProcAddr",
         reinterpret_cast<PFN_vkVoidFunction>(LowLatency_GetInstanceProcAddr)},

        {"vkCreateInstance",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::CreateInstance)},
        {"vkDestroyInstance",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::DestroyInstance)},
    };

static const auto device_functions =
    std::unordered_map<std::string_view, const PFN_vkVoidFunction>{
        {"vkGetDeviceProcAddr",
         reinterpret_cast<PFN_vkVoidFunction>(LowLatency_GetDeviceProcAddr)},

        {"vkCreateDevice",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::CreateDevice)},
        {"vkDestroyDevice",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::DestroyDevice)},

        {"vkGetDeviceQueue",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::GetDeviceQueue)},
        {"vkGetDeviceQueue2",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::GetDeviceQueue2)},

        {"vkQueueSubmit",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::vkQueueSubmit)},
        {"vkQueueSubmit2",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::vkQueueSubmit2)},

        {"vkQueuePresentKHR",
         reinterpret_cast<PFN_vkVoidFunction>(low_latency::vkQueuePresentKHR)},
    };

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
LowLatency_GetDeviceProcAddr(VkDevice device, const char* const pName) {

    if (const auto it = device_functions.find(pName);
        it != std::end(device_functions)) {

        return it->second;
    }

    const auto lock = std::scoped_lock{low_latency::layer_context.mutex};

    using namespace low_latency;
    const auto& context = layer_context.get_context<DeviceContext>(device);
    return context.vtable.GetDeviceProcAddr(device, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
LowLatency_GetInstanceProcAddr(VkInstance instance, const char* const pName) {

    for (const auto& functions : {device_functions, instance_functions}) {

        if (const auto it = functions.find(pName); it != std::end(functions)) {
            return it->second;
        }
    }

    const auto lock = std::scoped_lock{low_latency::layer_context.mutex};

    using namespace low_latency;
    const auto& context = layer_context.get_context<InstanceContext>(instance);
    return context.vtable.GetInstanceProcAddr(instance, pName);
}