#include "layer.hh"

#include <iostream>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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
#include "timestamp_pool.hh"

namespace low_latency {

namespace {

LayerContext layer_context;

} // namespace

template <typename T>
static T* find_next(void* const head, const VkStructureType& stype) {
    for (auto i = reinterpret_cast<VkBaseOutStructure*>(head); i;
         i = i->pNext) {

        if (i->sType != stype) {
            continue;
        }
        return reinterpret_cast<T*>(i);
    }
    return nullptr;
}

template <typename T>
static const T* find_next(const void* const head,
                          const VkStructureType& stype) {
    for (auto i = reinterpret_cast<const VkBaseInStructure*>(head); i;
         i = i->pNext) {

        if (i->sType != stype) {
            continue;
        }
        return reinterpret_cast<const T*>(i);
    }
    return nullptr;
}

template <typename T>
static const T* find_link(const void* head, const VkStructureType& stype) {
    for (auto info = find_next<T>(head, stype); info;
         info = find_next<T>(info->pNext, stype)) {

        if (info->function != VK_LAYER_LINK_INFO) {
            continue;
        }
        return reinterpret_cast<const T*>(info);
    }
    return nullptr;
}

static VKAPI_ATTR VkResult VKAPI_CALL
CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
               const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {

    const auto link_info = find_link<VkLayerInstanceCreateInfo>(
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
    const_cast<VkLayerInstanceCreateInfo*>(link_info)->u.pLayerInfo =
        link_info->u.pLayerInfo->pNext;

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

#define INSTANCE_VTABLE_LOAD(name)                                             \
    vtable.name = reinterpret_cast<PFN_vk##name>(gipa(*pInstance, "vk" #name))
    auto vtable = VkuInstanceDispatchTable{};
    INSTANCE_VTABLE_LOAD(DestroyInstance);
    INSTANCE_VTABLE_LOAD(EnumeratePhysicalDevices);
    INSTANCE_VTABLE_LOAD(GetPhysicalDeviceProperties);
    INSTANCE_VTABLE_LOAD(GetInstanceProcAddr);
    INSTANCE_VTABLE_LOAD(CreateDevice);
    INSTANCE_VTABLE_LOAD(EnumerateDeviceExtensionProperties);
    INSTANCE_VTABLE_LOAD(GetPhysicalDeviceQueueFamilyProperties2);
    INSTANCE_VTABLE_LOAD(GetPhysicalDeviceFeatures2);
#undef INSTANCE_VTABLE_LOAD

    const auto lock = std::scoped_lock{layer_context.mutex};
    assert(!layer_context.contexts.contains(key));

    layer_context.contexts.try_emplace(
        key, std::make_shared<InstanceContext>(*pInstance, std::move(vtable)));

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {

    const auto destroy_instance_func = [&]() -> auto {
        const auto context = layer_context.get_context(instance);
        const auto lock = std::scoped_lock{layer_context.mutex};

        // Erase our physical devices owned by this instance from the global
        // context.
        for (const auto& [key, _] : context->phys_devices) {
            assert(layer_context.contexts.erase(key));
        }

        const auto key = layer_context.get_key(instance);
        assert(layer_context.contexts.erase(key));

        // Should be the last ptr now like DestroyDevice.
        assert(context.unique());
        return context->vtable.DestroyInstance;
    }();

    destroy_instance_func(instance, allocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(
    VkInstance instance, std::uint32_t* count, VkPhysicalDevice* devices) {

    const auto context = layer_context.get_context(instance);

    if (const auto result =
            context->vtable.EnumeratePhysicalDevices(instance, count, devices);
        !devices || !count || result != VK_SUCCESS) {

        return result;
    }

    const auto lock = std::scoped_lock{layer_context.mutex};
    for (const auto& device : std::span{devices, *count}) {
        const auto key = layer_context.get_key(device);
        const auto [iter, inserted] =
            layer_context.contexts.try_emplace(key, nullptr);

        if (inserted) {
            iter->second =
                std::make_shared<PhysicalDeviceContext>(*context, device);
        }
    }

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {

    const auto create_info = find_link<VkLayerDeviceCreateInfo>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!create_info || !create_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const auto gipa = create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gdpa = create_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    if (!gipa || !gdpa) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const_cast<VkLayerDeviceCreateInfo*>(create_info)->u.pLayerInfo =
        create_info->u.pLayerInfo->pNext;

    const auto physical_device_context =
        layer_context.get_context(physical_device);
    auto& instance_context = physical_device_context->instance;

    const auto next_extensions =
        [&]() -> std::optional<std::vector<const char*>> {
        const auto enumerate_device_extensions =
            reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                gipa(instance_context.instance,
                     "vkEnumerateDeviceExtensionProperties"));
        if (!enumerate_device_extensions) {
            return std::nullopt;
        }

        auto count = std::uint32_t{};
        if (enumerate_device_extensions(physical_device, nullptr, &count,
                                        nullptr) != VK_SUCCESS) {

            return std::nullopt;
        }

        auto supported_extensions = std::vector<VkExtensionProperties>(count);
        if (enumerate_device_extensions(physical_device, nullptr, &count,
                                        std::data(supported_extensions)) !=
            VK_SUCCESS) {

            return std::nullopt;
        }

        auto next_extensions = std::vector<const char*>{};

        std::ranges::copy(std::span{pCreateInfo->ppEnabledExtensionNames,
                                    pCreateInfo->enabledExtensionCount},
                          std::back_inserter(next_extensions));

        const auto wanted_extensions = {
            VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
            VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
            VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME};

        for (const auto& wanted : wanted_extensions) {

            if (std::ranges::any_of(
                    next_extensions, [&](const auto& next_extension) {
                        return !std::strcmp(next_extension, wanted);
                    })) {

                continue; // Already included, ignore it.
            }

            if (std::ranges::none_of(
                    supported_extensions, [&](const auto& supported_extension) {
                        return !std::strcmp(supported_extension.extensionName,
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

    const auto create_device = instance_context.vtable.CreateDevice;
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

#define DEVICE_VTABLE_LOAD(name)                                               \
    vtable.name = reinterpret_cast<PFN_vk##name>(gdpa(*pDevice, "vk" #name))
    auto vtable = VkuDeviceDispatchTable{};
    DEVICE_VTABLE_LOAD(GetDeviceProcAddr);
    DEVICE_VTABLE_LOAD(DestroyDevice);
    DEVICE_VTABLE_LOAD(GetDeviceQueue);
    DEVICE_VTABLE_LOAD(QueueSubmit);
    DEVICE_VTABLE_LOAD(CreateQueryPool);
    DEVICE_VTABLE_LOAD(DestroyQueryPool);
    DEVICE_VTABLE_LOAD(GetQueryPoolResults);
    DEVICE_VTABLE_LOAD(CreateCommandPool);
    DEVICE_VTABLE_LOAD(DestroyCommandPool);
    DEVICE_VTABLE_LOAD(AllocateCommandBuffers);
    DEVICE_VTABLE_LOAD(FreeCommandBuffers);
    DEVICE_VTABLE_LOAD(BeginCommandBuffer);
    DEVICE_VTABLE_LOAD(EndCommandBuffer);
    DEVICE_VTABLE_LOAD(ResetCommandBuffer);
    DEVICE_VTABLE_LOAD(CmdDraw);
    DEVICE_VTABLE_LOAD(CmdDrawIndexed);
    DEVICE_VTABLE_LOAD(CmdResetQueryPool);
    DEVICE_VTABLE_LOAD(GetDeviceQueue2);
    DEVICE_VTABLE_LOAD(QueueSubmit2);
    DEVICE_VTABLE_LOAD(AcquireNextImageKHR);
    DEVICE_VTABLE_LOAD(QueuePresentKHR);
    DEVICE_VTABLE_LOAD(AcquireNextImage2KHR);
    DEVICE_VTABLE_LOAD(CmdWriteTimestamp2KHR);
    DEVICE_VTABLE_LOAD(QueueSubmit2KHR);
    DEVICE_VTABLE_LOAD(GetCalibratedTimestampsKHR);
    DEVICE_VTABLE_LOAD(ResetQueryPoolEXT);
#undef DEVICE_VTABLE_LOAD

    const auto physical_context = layer_context.get_context(physical_device);

    const auto key = layer_context.get_key(*pDevice);
    const auto lock = std::scoped_lock{layer_context.mutex};
    assert(!layer_context.contexts.contains(key));

    layer_context.contexts.try_emplace(
        key,
        std::make_shared<DeviceContext>(instance_context, *physical_context,
                                        *pDevice, std::move(vtable)));

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {

    const auto destroy_device_func = [&]() -> auto {
        const auto device_context = layer_context.get_context(device);

        const auto func = device_context->vtable.DestroyDevice;
        const auto lock = std::scoped_lock{layer_context.mutex};
        // Remove all owned queues from our global context pool.
        for (const auto& [queue, _] : device_context->queues) {
            const auto key = layer_context.get_key(queue);
            assert(layer_context.contexts.erase(key));
        }

        const auto key = layer_context.get_key(device);
        assert(layer_context.contexts.erase(key));

        // should be the last shared ptr now, so its destructor can be called.
        // the destructor should expect its owned queues to be unique as well!
        assert(device_context.unique());

        return func;
    }();

    destroy_device_func(device, allocator);
}

static VKAPI_ATTR void VKAPI_CALL
GetDeviceQueue(VkDevice device, std::uint32_t queue_family_index,
               std::uint32_t queue_index, VkQueue* queue) {

    const auto device_context = layer_context.get_context(device);

    device_context->vtable.GetDeviceQueue(device, queue_family_index,
                                          queue_index, queue);
    if (!queue || !*queue) {
        return;
    }

    // Look in our layer context, which has everything. If we were able to
    // insert a nullptr key, then it didn't already exist so we should
    // construct a new one.
    const auto key = layer_context.get_key(*queue);
    const auto lock = std::scoped_lock{layer_context.mutex};
    const auto [it, inserted] = layer_context.contexts.try_emplace(key);
    if (inserted) {
        it->second = std::make_shared<QueueContext>(*device_context, *queue,
                                                    queue_family_index);
    }

    // it->second should be QueueContext, also it might already be there
    // but this is expected.
    const auto ptr = std::dynamic_pointer_cast<QueueContext>(it->second);
    assert(ptr);
    device_context->queues.emplace(*queue, ptr);
}

// Identical logic to gdq so some amount of duplication, we can't assume gdq1 is
// available apparently, what do I know?
static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {

    const auto device_context = layer_context.get_context(device);

    device_context->vtable.GetDeviceQueue2(device, info, queue);
    if (!queue || !*queue) {
        return;
    }

    const auto key = layer_context.get_key(*queue);
    const auto lock = std::scoped_lock{layer_context.mutex};
    const auto [it, inserted] = layer_context.contexts.try_emplace(key);
    if (inserted) {
        it->second = std::make_shared<QueueContext>(*device_context, *queue,
                                                    info->queueFamilyIndex);
    }

    const auto ptr = std::dynamic_pointer_cast<QueueContext>(it->second);
    assert(ptr);
    device_context->queues.emplace(*queue, ptr);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, std::uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, std::uint32_t* pImageIndex) {

    const auto context = layer_context.get_context(device);
    if (const auto result = context->vtable.AcquireNextImageKHR(
            device, swapchain, timeout, semaphore, fence, pImageIndex);
        result != VK_SUCCESS) {

        return result;
    }

    context->notify_acquire(swapchain, *pImageIndex, semaphore);

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(
    VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo,
    std::uint32_t* pImageIndex) {

    const auto context = layer_context.get_context(device);
    if (const auto result = context->vtable.AcquireNextImage2KHR(
            device, pAcquireInfo, pImageIndex);
        result != VK_SUCCESS) {

        return result;
    }

    context->notify_acquire(pAcquireInfo->swapchain, *pImageIndex,
                            pAcquireInfo->semaphore);

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(VkQueue queue, std::uint32_t submit_count,
              const VkSubmitInfo* submit_infos, VkFence fence) {

    const auto& queue_context = layer_context.get_context(queue);
    const auto& vtable = queue_context->device_context.vtable;

    if (!submit_count) { // no-op submit we shouldn't worry about
        return vtable.QueueSubmit(queue, submit_count, submit_infos, fence);
    }

    if (!queue_context->should_inject_timestamps()) {
        return vtable.QueueSubmit(queue, submit_count, submit_infos, fence);
    }

    // What's happening here?
    // We are making a very modest modification to all vkQueueSubmits where we
    // inject a start and end timestamp query command buffer that writes when
    // the GPU started and finished work for each submission. It's important to
    // note that we deliberately do *NOT* use or modify any semaphores
    // as a mechanism to signal completion or the availability of these submits
    // for multiple reasons:
    //     1. Modifying semaphores (particuarly in vkQueueSubmit1) is ANNOYING
    //        done correctly. The pNext chain is const and difficult to modify
    //        without traversing the entire thing and doing surgical deep copies
    //        and patches for multiple pNext's sType's. It's easier to leave it
    //        alone.
    //     2. Semaphores only signal at the end of their work, so we cannot use
    //        them as a mechanism to know if work has started without doing
    //        another dummy submission. This adds complexity and also skews our
    //        timestamps slightly.
    //     3. Semaphores can be waited which sounds nice in theory, but in my
    //        own testing waiting on semaphores can cause scheduling issues and
    //        cause wakeups as late as 1ms from when it was signalled, which is
    //        unbelievably bad if we're trying to do frame pacing. This means
    //        we are going to have to do a spinlock poll anyway.
    //     4. Guess what info we need? Timestamp information. Guess what
    //        supports polling of an availability bit? Timestamp information.
    //        Why bother with semaphores at all then? Polling a semaphore might
    //        be faster, but the difference appears to be negligible.

    using cbs_t = std::vector<VkCommandBuffer>;
    auto next_submits = std::vector<VkSubmitInfo>{};

    // We're making modifications to multiple vkQueueSubmits. These have raw
    // pointers to our command buffer arrays - of which the position in memory
    // of can change on vector reallocation. So we use unique_ptrs here.
    auto next_cbs = std::vector<std::unique_ptr<cbs_t>>{};

    // notify_submit() should take copies of these shared_ptrs and store
    // them for the duration of our call, but saving them here is a bit
    // more explicit + insurance if that changes.
    auto handles = std::vector<std::shared_ptr<TimestampPool::Handle>>{};

    const auto now = std::chrono::steady_clock::now();

    std::ranges::transform(
        std::span{submit_infos, submit_count}, std::back_inserter(next_submits),
        [&](const auto& submit) {
            const auto head_handle = queue_context->timestamp_pool->acquire();
            const auto tail_handle = queue_context->timestamp_pool->acquire();
            head_handle->setup_command_buffers(*tail_handle, *queue_context);
            queue_context->notify_submit(submit, head_handle, tail_handle, now);

            handles.emplace_back(head_handle);
            handles.emplace_back(tail_handle);
            next_cbs.emplace_back([&]() -> auto {
                auto cbs = std::make_unique<cbs_t>();
                cbs->push_back(head_handle->command_buffer);
                std::ranges::copy(std::span{submit.pCommandBuffers,
                                            submit.commandBufferCount},
                                  std::back_inserter(*cbs));
                cbs->push_back(tail_handle->command_buffer);
                return cbs;
            }());

            auto next_submit = submit;
            next_submit.pCommandBuffers = std::data(*next_cbs.back());
            next_submit.commandBufferCount = std::size(*next_cbs.back());
            return next_submit;
        });

    return vtable.QueueSubmit(queue, std::size(next_submits),
                              std::data(next_submits), fence);
}

// The logic for this function is identical to vkSubmitInfo.
static VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit2(VkQueue queue, std::uint32_t submit_count,
               const VkSubmitInfo2* submit_infos, VkFence fence) {

    const auto& queue_context = layer_context.get_context(queue);
    const auto& vtable = queue_context->device_context.vtable;

    if (!submit_count) {
        return vtable.QueueSubmit2(queue, submit_count, submit_infos, fence);
    }

    if (!queue_context->should_inject_timestamps()) {
        return vtable.QueueSubmit2(queue, submit_count, submit_infos, fence);
    }

    using cbs_t = std::vector<VkCommandBufferSubmitInfo>;
    auto next_submits = std::vector<VkSubmitInfo2>{};
    auto next_cbs = std::vector<std::unique_ptr<cbs_t>>{};
    auto handles = std::vector<std::shared_ptr<TimestampPool::Handle>>{};

    const auto now = std::chrono::steady_clock::now();

    std::ranges::transform(
        std::span{submit_infos, submit_count}, std::back_inserter(next_submits),
        [&](const auto& submit) {
            const auto head_handle = queue_context->timestamp_pool->acquire();
            const auto tail_handle = queue_context->timestamp_pool->acquire();
            head_handle->setup_command_buffers(*tail_handle, *queue_context);
            queue_context->notify_submit(submit, head_handle, tail_handle, now);

            handles.emplace_back(head_handle);
            handles.emplace_back(tail_handle);
            next_cbs.emplace_back([&]() -> auto {
                auto cbs = std::make_unique<cbs_t>();
                cbs->push_back(VkCommandBufferSubmitInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                    .commandBuffer = head_handle->command_buffer,
                });
                std::ranges::copy(std::span{submit.pCommandBufferInfos,
                                            submit.commandBufferInfoCount},
                                  std::back_inserter(*cbs));
                cbs->push_back(VkCommandBufferSubmitInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                    .commandBuffer = tail_handle->command_buffer,
                });
                return cbs;
            }());

            auto next_submit = submit;
            next_submit.pCommandBufferInfos = std::data(*next_cbs.back());
            next_submit.commandBufferInfoCount = std::size(*next_cbs.back());
            return next_submit;
        });

    return vtable.QueueSubmit2(queue, std::size(next_submits),
                               std::data(next_submits), fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit2KHR(VkQueue queue, std::uint32_t submit_count,
                  const VkSubmitInfo2* submit_info, VkFence fence) {
    // Just forward to low_latency::vkQueueSubmit2 here.
    return low_latency::vkQueueSubmit2(queue, submit_count, submit_info, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* present_info) {

    const auto queue_context = layer_context.get_context(queue);
    const auto& vtable = queue_context->device_context.vtable;

    if (const auto res = vtable.QueuePresentKHR(queue, present_info);
        res != VK_SUCCESS) {

        return res;
    }

    if (present_info) { // might not be needed
        queue_context->notify_present(*present_info);
    }

    queue_context->sleep_in_present();

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device, const char* pLayerName,
    std::uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {

    const auto physical_context = layer_context.get_context(physical_device);
    const auto& instance = physical_context->instance;
    const auto& vtable = instance.vtable;

    // Not asking about our layer - just forward it.
    if (!pLayerName || std::string_view{pLayerName} != LAYER_NAME) {
        return vtable.EnumerateDeviceExtensionProperties(
            physical_device, pLayerName, pPropertyCount, pProperties);
    }

    // !pProperties means they're querying how much space they need.
    if (!pProperties) {
        *pPropertyCount = 1;
        return VK_SUCCESS;
    }

    auto& count = *pPropertyCount;
    // Defensive - they gave us zero space to work with.
    if (!count) {
        return VK_INCOMPLETE;
    }

    pProperties[0] =
        VkExtensionProperties{.extensionName = VK_AMD_ANTI_LAG_EXTENSION_NAME,
                              .specVersion = VK_AMD_ANTI_LAG_SPEC_VERSION};
    count = 1;

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2* pFeatures) {

    const auto physical_context = layer_context.get_context(physical_device);
    const auto& vtable = physical_context->instance.vtable;

    vtable.GetPhysicalDeviceFeatures2(physical_device, pFeatures);

    if (const auto feature = find_next<VkPhysicalDeviceAntiLagFeaturesAMD>(
            pFeatures->pNext,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD);
        feature) {

        feature->antiLag = true;
    }
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2KHR(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2KHR* pFeatures) {
    // forward
    return low_latency::GetPhysicalDeviceFeatures2(physical_device, pFeatures);
}

static VKAPI_ATTR void VKAPI_CALL
AntiLagUpdateAMD(VkDevice device, const VkAntiLagDataAMD* pData) {
    std::cerr << "low_latency::AntiLagUpdateAMD\n";
    std::cerr << "    maxFPS: " << pData->maxFPS << '\n';
    std::cerr << "    mode: " << pData->mode << '\n';
    std::cerr << "    pPresentInfo: " << pData->pPresentationInfo->frameIndex
              << '\n';
    std::cerr << "        frameIndex: " << pData->pPresentationInfo->frameIndex
              << '\n';
    std::cerr << "        stage: " << pData->pPresentationInfo->stage << '\n';
}

} // namespace low_latency

using func_map_t = std::unordered_map<std::string_view, PFN_vkVoidFunction>;
#define HOOK_ENTRY(vk_name_literal, fn_sym)                                    \
    {vk_name_literal, reinterpret_cast<PFN_vkVoidFunction>(fn_sym)}
static const auto instance_functions = func_map_t{
    HOOK_ENTRY("vkCreateDevice", low_latency::CreateDevice),

    HOOK_ENTRY("vkGetInstanceProcAddr", LowLatency_GetInstanceProcAddr),
    HOOK_ENTRY("vkGetDeviceProcAddr", LowLatency_GetDeviceProcAddr),

    HOOK_ENTRY("vkEnumeratePhysicalDevices",
               low_latency::EnumeratePhysicalDevices),

    HOOK_ENTRY("vkCreateInstance", low_latency::CreateInstance),
    HOOK_ENTRY("vkDestroyInstance", low_latency::DestroyInstance),

    HOOK_ENTRY("vkEnumerateDeviceExtensionProperties",
               low_latency::EnumerateDeviceExtensionProperties),

    HOOK_ENTRY("vkGetPhysicalDeviceFeatures2",
               low_latency::GetPhysicalDeviceFeatures2),
    HOOK_ENTRY("vkGetPhysicalDeviceFeatures2KHR",
               low_latency::GetPhysicalDeviceFeatures2KHR),
};

static const auto device_functions = func_map_t{
    HOOK_ENTRY("vkGetDeviceProcAddr", LowLatency_GetDeviceProcAddr),

    HOOK_ENTRY("vkDestroyDevice", low_latency::DestroyDevice),

    HOOK_ENTRY("vkGetDeviceQueue", low_latency::GetDeviceQueue),
    HOOK_ENTRY("vkGetDeviceQueue2", low_latency::GetDeviceQueue2),

    HOOK_ENTRY("vkQueueSubmit", low_latency::vkQueueSubmit),
    HOOK_ENTRY("vkQueueSubmit2", low_latency::vkQueueSubmit2),
    HOOK_ENTRY("vkQueueSubmit2KHR", low_latency::vkQueueSubmit2KHR),

    HOOK_ENTRY("vkQueuePresentKHR", low_latency::vkQueuePresentKHR),

    HOOK_ENTRY("vkAcquireNextImageKHR", low_latency::vkAcquireNextImageKHR),
    HOOK_ENTRY("vkAcquireNextImage2KHR", low_latency::vkAcquireNextImage2KHR),

    HOOK_ENTRY("vkAntiLagUpdateAMD", low_latency::AntiLagUpdateAMD),
};
#undef HOOK_ENTRY

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
LowLatency_GetDeviceProcAddr(VkDevice device, const char* const pName) {
    if (!pName || !device) {
        return nullptr;
    }

    if (const auto it = device_functions.find(pName);
        it != std::end(device_functions)) {

        return it->second;
    }

    using namespace low_latency;
    const auto& vtable = layer_context.get_context(device)->vtable;
    return vtable.GetDeviceProcAddr(device, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
LowLatency_GetInstanceProcAddr(VkInstance instance, const char* const pName) {
    if (const auto it = instance_functions.find(pName);
        it != std::end(instance_functions)) {

        return it->second;
    }

    using namespace low_latency;
    const auto& vtable = layer_context.get_context(instance)->vtable;
    return vtable.GetInstanceProcAddr(instance, pName);
}
