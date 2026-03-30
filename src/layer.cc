#include "layer.hh"

#include <ranges>
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

#include "device_clock.hh"
#include "device_context.hh"
#include "helper.hh"
#include "instance_context.hh"
#include "layer_context.hh"
#include "queue_context.hh"
#include "timestamp_pool.hh"

namespace low_latency {

namespace {

LayerContext layer_context;

} // namespace

static VKAPI_ATTR VkResult VKAPI_CALL
CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
               const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {

    const auto link_info = find_link<VkLayerInstanceCreateInfo>(
        pCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);

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
    INSTANCE_VTABLE_LOAD(GetPhysicalDeviceProperties2);
    INSTANCE_VTABLE_LOAD(GetPhysicalDeviceProperties2KHR);
    INSTANCE_VTABLE_LOAD(GetInstanceProcAddr);
    INSTANCE_VTABLE_LOAD(CreateDevice);
    INSTANCE_VTABLE_LOAD(EnumerateDeviceExtensionProperties);
    INSTANCE_VTABLE_LOAD(GetPhysicalDeviceQueueFamilyProperties2);
    INSTANCE_VTABLE_LOAD(GetPhysicalDeviceFeatures2);
    INSTANCE_VTABLE_LOAD(GetPhysicalDeviceSurfaceCapabilities2KHR);
#undef INSTANCE_VTABLE_LOAD

    const auto lock = std::scoped_lock{layer_context.mutex};
    assert(!layer_context.contexts.contains(key));

    layer_context.contexts.try_emplace(
        key, std::make_shared<InstanceContext>(layer_context, *pInstance,
                                               std::move(vtable)));

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
            assert(layer_context.contexts.contains(key));
            layer_context.contexts.erase(key);
        }

        const auto key = layer_context.get_key(instance);
        assert(layer_context.contexts.contains(key));
        layer_context.contexts.erase(key);

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

    const auto enabled_extensions =
        std::span{pCreateInfo->ppEnabledExtensionNames,
                  pCreateInfo->enabledExtensionCount};

    const auto requested = std::unordered_set<std::string_view>(
        std::from_range, enabled_extensions);

    const auto was_capability_requested =
        requested.contains(!layer_context.should_expose_reflex
                               ? VK_AMD_ANTI_LAG_EXTENSION_NAME
                               : VK_NV_LOW_LATENCY_2_EXTENSION_NAME);

    const auto context = layer_context.get_context(physical_device);
    if (was_capability_requested && !context->supports_required_extensions) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const auto create_info = find_link<VkLayerDeviceCreateInfo>(
        pCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!create_info || !create_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const auto gdpa = create_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    if (!gdpa) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const_cast<VkLayerDeviceCreateInfo*>(create_info)->u.pLayerInfo =
        create_info->u.pLayerInfo->pNext;

    // Build a next extensions vector from what they have requested.
    const auto next_extensions = [&]() -> std::vector<const char*> {
        auto next_extensions = std::vector(std::from_range, enabled_extensions);

        if (!was_capability_requested) {
            return next_extensions;
        }

        // Only append the extra extension if it wasn't already asked for.
        for (const auto& wanted : PhysicalDeviceContext::required_extensions) {
            if (!requested.contains(wanted)) {
                next_extensions.push_back(wanted);
            }
        }

        return next_extensions;
    }();

    const auto next_create_info = [&]() -> VkDeviceCreateInfo {
        auto next_pCreateInfo = *pCreateInfo;
        next_pCreateInfo.ppEnabledExtensionNames = std::data(next_extensions);
        next_pCreateInfo.enabledExtensionCount =
            static_cast<std::uint32_t>(std::size(next_extensions));
        return next_pCreateInfo;
    }();

    if (const auto result = context->instance.vtable.CreateDevice(
            physical_device, &next_create_info, pAllocator, pDevice);
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
    DEVICE_VTABLE_LOAD(SignalSemaphore);
    DEVICE_VTABLE_LOAD(CreateSwapchainKHR);
    DEVICE_VTABLE_LOAD(DestroySwapchainKHR);
#undef DEVICE_VTABLE_LOAD

    const auto key = layer_context.get_key(*pDevice);
    const auto lock = std::scoped_lock{layer_context.mutex};

    assert(!layer_context.contexts.contains(key));
    layer_context.contexts.try_emplace(
        key, std::make_shared<DeviceContext>(context->instance, *context,
                                             *pDevice, was_capability_requested,
                                             std::move(vtable)));

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
            assert(layer_context.contexts.contains(key));
            layer_context.contexts.erase(key);
        }

        const auto key = layer_context.get_key(device);
        assert(layer_context.contexts.contains(key));
        layer_context.contexts.erase(key);

        // Should be the last shared ptr now, so its destructor can be called.
        // The destructor should expect its owned queues to be unique as well.
        assert(device_context.unique());

        return func;
    }();

    destroy_device_func(device, allocator);
}

static VKAPI_ATTR void VKAPI_CALL
GetDeviceQueue(VkDevice device, std::uint32_t queue_family_index,
               std::uint32_t queue_index, VkQueue* queue) {

    const auto context = layer_context.get_context(device);

    // Get device queue, unlike CreateDevice or CreateInstance, can be
    // called multiple times to return the same queue object. Our insertion
    // handling has to be a little different where we account for this.
    context->vtable.GetDeviceQueue(device, queue_family_index, queue_index,
                                   queue);
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
        it->second = std::make_shared<QueueContext>(*context, *queue,
                                                    queue_family_index);
    }

    // it->second should be QueueContext, also it might already be there.
    const auto ptr = std::dynamic_pointer_cast<QueueContext>(it->second);
    assert(ptr);
    context->queues.emplace(*queue, ptr);
}

// Identical logic to gdq1.
static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {

    const auto context = layer_context.get_context(device);

    context->vtable.GetDeviceQueue2(device, info, queue);
    if (!queue || !*queue) {
        return;
    }

    const auto key = layer_context.get_key(*queue);
    const auto lock = std::scoped_lock{layer_context.mutex};
    const auto [it, inserted] = layer_context.contexts.try_emplace(key);
    if (inserted) {
        it->second = std::make_shared<QueueContext>(*context, *queue,
                                                    info->queueFamilyIndex);
    }

    const auto ptr = std::dynamic_pointer_cast<QueueContext>(it->second);
    assert(ptr);
    context->queues.emplace(*queue, ptr);
}

static VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(VkQueue queue, std::uint32_t submit_count,
              const VkSubmitInfo* submit_infos, VkFence fence) {

    const auto context = layer_context.get_context(queue);
    const auto& vtable = context->device.vtable;

    if (!submit_count || !context->should_inject_timestamps()) {
        return vtable.QueueSubmit(queue, submit_count, submit_infos, fence);
    }

    // What's happening here?
    // We are making a very modest modification to all vkQueueSubmits where we
    // inject a start and end timestamp query command buffer that writes when
    // the GPU started and finished work for each submission. Note, we do *NOT*
    // use or modify any semaphores as a mechanism to signal completion or the
    // availability of these submits for multiple reasons:
    //     1. Modifying semaphores (particuarly in vkQueueSubmit1) is ANNOYING
    //        done correctly. The pNext chain is const and difficult to modify
    //        without traversing the entire thing and doing surgical deep copies
    //        and patches for multiple pNext's sType's. It's easier to leave it
    //        alone. If we do edit them it's either a maintenance nightmare or
    //        an illegal const cast timebomb that breaks valid vulkan
    //        applications that pass truly read only vkSubmitInfo->pNext's.
    //     2. Semaphores only signal at the end of their work, so we cannot use
    //        them as a mechanism to know if work has started without doing
    //        another dummy submission. If we did this it adds complexity and
    //        also might skew our timestamps slightly as they wouldn't be a part
    //        of the submission which contained those command buffers.
    //     3. Timestamps support querying if their work has started/ended
    //        as long as we use the vkHostQueryReset extension to reset them
    //        before we consider them queryable. This means we don't need a
    //        'is it valid to query my timestamps' timeline semaphore.
    //     4. The performance impact of using semaphores vs timestamps is
    //        negligible.

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

    const auto now = DeviceClock::now();

    std::ranges::transform(
        std::span{submit_infos, submit_count}, std::back_inserter(next_submits),
        [&](const auto& submit) {
            const auto head_handle = context->timestamp_pool->acquire();
            const auto tail_handle = context->timestamp_pool->acquire();
            head_handle->setup_command_buffers(*tail_handle, *context);

            context->notify_submit(extract_present_id(submit), head_handle,
                                   tail_handle, now);

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
            next_submit.commandBufferCount =
                static_cast<std::uint32_t>(std::size(*next_cbs.back()));
            return next_submit;
        });

    return vtable.QueueSubmit(
        queue, static_cast<std::uint32_t>(std::size(next_submits)),
        std::data(next_submits), fence);
}

// The logic for this function is identical to vkSubmitInfo.
static VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit2(VkQueue queue, std::uint32_t submit_count,
               const VkSubmitInfo2* submit_infos, VkFence fence) {

    const auto context = layer_context.get_context(queue);
    const auto& vtable = context->device.vtable;

    if (!submit_count || !context->should_inject_timestamps()) {
        return vtable.QueueSubmit2(queue, submit_count, submit_infos, fence);
    }

    using cbs_t = std::vector<VkCommandBufferSubmitInfo>;
    auto next_submits = std::vector<VkSubmitInfo2>{};
    auto next_cbs = std::vector<std::unique_ptr<cbs_t>>{};
    auto handles = std::vector<std::shared_ptr<TimestampPool::Handle>>{};

    const auto now = DeviceClock::now();

    std::ranges::transform(
        std::span{submit_infos, submit_count}, std::back_inserter(next_submits),
        [&](const auto& submit) {
            const auto head_handle = context->timestamp_pool->acquire();
            const auto tail_handle = context->timestamp_pool->acquire();
            head_handle->setup_command_buffers(*tail_handle, *context);

            context->notify_submit(extract_present_id(submit), head_handle,
                                   tail_handle, now);

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
            next_submit.commandBufferInfoCount =
                static_cast<std::uint32_t>(std::size(*next_cbs.back()));
            return next_submit;
        });

    return vtable.QueueSubmit2(
        queue, static_cast<std::uint32_t>(std::size(next_submits)),
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

    const auto context = layer_context.get_context(queue);
    const auto& vtable = context->device.vtable;

    if (const auto res = vtable.QueuePresentKHR(queue, present_info);
        res != VK_SUCCESS) {

        return res;
    }

    const auto pid = find_next<VkPresentIdKHR>(
        present_info, VK_STRUCTURE_TYPE_PRESENT_ID_KHR);

    for (auto i = std::uint32_t{0}; i < present_info->swapchainCount; ++i) {
        const auto& swapchain = present_info->pSwapchains[i];

        // For VK_AMD_anti_lag, providing a pPresentId isn't part of the spec.
        // So we just set it to 0 if it isn't provided.
        const auto present_id = pid ? pid->pPresentIds[i] : 0;

        context->notify_present(swapchain, present_id);
    }

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device, const char* pLayerName,
    std::uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {

    const auto context = layer_context.get_context(physical_device);
    const auto& vtable = context->instance.vtable;

    // This used to be a bit less complicated because we could rely on the
    // loader mashing everything together provided we gave our anti lag
    // extension in our JSON manifest. We now try to spoof nvidia and what we
    // provide is dynamic. The JSON isn't dynamic. So we can't use that anymore!

    // Simplest case, they're not asking about us so we can happily forward it.
    if (pLayerName && std::string_view{pLayerName} != LAYER_NAME) {
        return vtable.EnumerateDeviceExtensionProperties(
            physical_device, pLayerName, pPropertyCount, pProperties);
    }

    // If we're exposing reflex we want to provide their extension instead.
    const auto extension_properties = [&]() -> VkExtensionProperties {
        if (context->instance.layer.should_expose_reflex) {
            return {.extensionName = VK_NV_LOW_LATENCY_2_EXTENSION_NAME,
                    .specVersion = VK_NV_LOW_LATENCY_2_SPEC_VERSION};
        }
        return {.extensionName = VK_AMD_ANTI_LAG_EXTENSION_NAME,
                .specVersion = VK_AMD_ANTI_LAG_SPEC_VERSION};
    }();

    if (pLayerName) {
        // This query is for our layer specifically.
        if (!pProperties) {
            *pPropertyCount = 1;
            return VK_SUCCESS;
        }

        if (!*pPropertyCount) {
            return VK_INCOMPLETE;
        }

        pProperties[0] = extension_properties;
        *pPropertyCount = 1;

        return VK_SUCCESS;
    }

    auto target_count = std::uint32_t{0};
    if (const auto result = vtable.EnumerateDeviceExtensionProperties(
            physical_device, nullptr, &target_count, nullptr);
        result != VK_SUCCESS) {

        return result;
    }
    target_count += 1;

    if (!pProperties) {
        *pPropertyCount = target_count;
        return VK_SUCCESS;
    }

    auto written = *pPropertyCount;
    if (const auto result = vtable.EnumerateDeviceExtensionProperties(
            physical_device, nullptr, &written, pProperties);
        result != VK_SUCCESS) {

        return result;
    }

    if (*pPropertyCount < target_count) {
        return VK_INCOMPLETE;
    }

    pProperties[target_count - 1] = extension_properties;
    *pPropertyCount = target_count;

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2* pFeatures) {

    const auto context = layer_context.get_context(physical_device);
    const auto& vtable = context->instance.vtable;

    vtable.GetPhysicalDeviceFeatures2(physical_device, pFeatures);

    // We're going to use this feature for both VK_AMD_anti_lag and
    // VK_NV_low_latency2. It simplifies things a bit if we share a code path.
    if (const auto pidf = find_next<VkPhysicalDevicePresentIdFeaturesKHR>(
            pFeatures,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
        pidf) {

        pidf->presentId = true;
    }

    // Don't provide AntiLag if we're exposing reflex - VK_NV_low_latency2 uses
    // VkSurfaceCapabilities2KHR to determine if a surface is capable of reflex
    // instead of AMD's physical device switch found here.
    if (context->instance.layer.should_expose_reflex) {
        return;
    }

    if (const auto alf = find_next<VkPhysicalDeviceAntiLagFeaturesAMD>(
            pFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD);
        alf) {

        alf->antiLag = context->supports_required_extensions;
    }
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2KHR(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2KHR* pFeatures) {

    return GetPhysicalDeviceFeatures2(physical_device, pFeatures);
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(
    VkPhysicalDevice physical_device, VkPhysicalDeviceProperties* pProperties) {

    const auto context = layer_context.get_context(physical_device);
    const auto& vtable = context->instance.vtable;

    vtable.GetPhysicalDeviceProperties(physical_device, pProperties);

    if (layer_context.should_spoof_nvidia) {
        pProperties->vendorID = LayerContext::NVIDIA_VENDOR_ID;
        pProperties->deviceID = LayerContext::NVIDIA_DEVICE_ID;

        // Most games seem happy without doing this, but I don't see why we
        // shouldn't. I could see an application checking this.
        std::strncpy(pProperties->deviceName, LayerContext::NVIDIA_DEVICE_NAME,
                     VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);
    }
}

// Identical logic to GetPhysicalDeviceProperties.
static VKAPI_ATTR void VKAPI_CALL
GetPhysicalDeviceProperties2(VkPhysicalDevice physical_device,
                             VkPhysicalDeviceProperties2* pProperties) {

    const auto context = layer_context.get_context(physical_device);
    const auto& vtable = context->instance.vtable;

    vtable.GetPhysicalDeviceProperties2(physical_device, pProperties);

    if (layer_context.should_spoof_nvidia) {
        pProperties->properties.vendorID = LayerContext::NVIDIA_VENDOR_ID;
        pProperties->properties.deviceID = LayerContext::NVIDIA_DEVICE_ID;
        std::strncpy(pProperties->properties.deviceName,
                     LayerContext::NVIDIA_DEVICE_NAME,
                     VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);
    }
}

static VKAPI_ATTR void VKAPI_CALL
GetPhysicalDeviceProperties2KHR(VkPhysicalDevice physical_device,
                                VkPhysicalDeviceProperties2* pProperties) {
    return GetPhysicalDeviceProperties2(physical_device, pProperties);
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    VkSurfaceCapabilities2KHR* pSurfaceCapabilities) {

    const auto context = layer_context.get_context(physical_device);
    const auto& vtable = context->instance.vtable;

    vtable.GetPhysicalDeviceSurfaceCapabilities2KHR(
        physical_device, pSurfaceInfo, pSurfaceCapabilities);

    // Don't do this unless we're spoofing nvidia.
    if (!context->instance.layer.should_expose_reflex) {
        return;
    }

    const auto lsc = find_next<VkLatencySurfaceCapabilitiesNV>(
        pSurfaceCapabilities,
        VK_STRUCTURE_TYPE_LATENCY_SURFACE_CAPABILITIES_NV);
    if (!lsc) {
        return;
    }

    // I eyeballed these - there might be more that we can support.
    const auto supported_modes = std::vector<VkPresentModeKHR>{
        VK_PRESENT_MODE_IMMEDIATE_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_FIFO_KHR,
    };
    const auto num_supported_modes =
        static_cast<std::uint32_t>(std::size(supported_modes));

    // They're asking how many we want to return.
    if (!lsc->pPresentModes) {
        lsc->presentModeCount = num_supported_modes;
        return;
    }

    // Finally we can write what surfaces are capable.
    const auto num_to_write =
        std::min(lsc->presentModeCount, num_supported_modes);

    std::ranges::copy_n(std::begin(supported_modes), num_to_write,
                        lsc->pPresentModes);

    lsc->presentModeCount = num_to_write;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {

    const auto context = layer_context.get_context(device);

    if (const auto result = context->vtable.CreateSwapchainKHR(
            device, pCreateInfo, pAllocator, pSwapchain);
        result != VK_SUCCESS) {

        return result;
    }

    // VK_NV_low_latency2 allows a swapchain to be created with the low latency
    // mode already on via VkSwapchainLatencyCreateInfoNV.
    auto was_low_latency_requested = false;
    if (const auto slci = find_next<VkSwapchainLatencyCreateInfoNV>(
            pCreateInfo, VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV);
        slci) {

        was_low_latency_requested = slci->latencyModeEnable;
    }

    const auto [_, did_emplace] = context->swapchain_monitors.try_emplace(
        *pSwapchain, *context, was_low_latency_requested);
    assert(did_emplace);

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                    const VkAllocationCallbacks* pAllocator) {
    const auto context = layer_context.get_context(device);

    assert(context->swapchain_monitors.contains(swapchain));
    context->swapchain_monitors.erase(swapchain);

    context->vtable.DestroySwapchainKHR(device, swapchain, pAllocator);
}

static VKAPI_ATTR void VKAPI_CALL
AntiLagUpdateAMD(VkDevice device, const VkAntiLagDataAMD* pData) {
    const auto context = layer_context.get_context(device);
    assert(pData);

    // AL2 is a synchronous while NVIDIA's low_latencty2 is asynchronous.
    // It's difficult to model an asynchronous impl inside a synchronous impl,
    // but it's easy to do the inverse. AMD's extension piggybacks on NVIDIA's
    // more complicated implementation.

    const auto present_delay = [&]() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(1s / pData->maxFPS);
    }();

    context->update_params(std::nullopt, present_delay,
                           (pData->mode == VK_ANTI_LAG_MODE_ON_AMD));

    if (!pData->pPresentationInfo ||
        pData->pPresentationInfo->stage != VK_ANTI_LAG_STAGE_INPUT_AMD) {

        return;
    }

    // VK_AMD_anti_lag doesn't provide a swapchain, so we can't map it to
    // a queue. Our previous implementation used the last queue that presented
    // and made sure that at least that one completed. I think it's more robust
    // to make sure they all complete.
    for (auto& iter : context->swapchain_monitors) {
        iter.second.wait_until();
    }
}

VkResult LatencySleepNV(VkDevice device, VkSwapchainKHR swapchain,
                        const VkLatencySleepInfoNV* pSleepInfo) {

    const auto context = layer_context.get_context(device);
    assert(pSleepInfo);

    // We're associating an application-provided timeline semaphore + value with
    // a swapchain that says 'signal me when we should move past input'.
    auto& swapchain_monitor = [&]() -> auto& {
        const auto iter = context->swapchain_monitors.find(swapchain);
        assert(iter != std::end(context->swapchain_monitors));
        return iter->second;
    }();

    // Tell our swapchain monitor that if they want us to proceed they should
    // signal this semaphore.
    swapchain_monitor.notify_semaphore(pSleepInfo->signalSemaphore,
                                       pSleepInfo->value);

    return VK_SUCCESS;
}

void QueueNotifyOutOfBandNV(VkQueue queue,
                            const VkOutOfBandQueueTypeInfoNV* pQueueTypeInfo) {

    // Kind of interesting how you can't turn it back on once it's turned off.
    // Also I really have no idea why pQueueTypeInfo's VkOutOfBandQueueTypeNV
    // enum even exists (I guess we will find out later when nothing works).
    const auto context = layer_context.get_context(queue);

    context->should_ignore_latency = true;
}

VkResult SetLatencySleepModeNV(VkDevice device, VkSwapchainKHR swapchain,
                               const VkLatencySleepModeInfoNV* pSleepModeInfo) {
    const auto context = layer_context.get_context(device);

    if (pSleepModeInfo) {
        context->update_params(
            swapchain,
            std::chrono::milliseconds{pSleepModeInfo->minimumIntervalUs},
            pSleepModeInfo->lowLatencyMode);
    } else {
        // If pSleepModeInfo is nullptr, it means no delay and no low latency.
        context->update_params(swapchain, std::chrono::milliseconds{0}, false);
    }

    return VK_SUCCESS;
}

void SetLatencyMarkerNV(VkDevice device, VkSwapchainKHR swapchain,
                        const VkSetLatencyMarkerInfoNV* pLatencyMarkerInfo) {
    // STUB
}

void GetLatencyTimingsNV(VkDevice device, VkSwapchainKHR swapchain,
                         VkGetLatencyMarkerInfoNV* pLatencyMarkerInfo) {
    // STUB
}

} // namespace low_latency

// This is a bit of template hackery which generates a wrapper function for each
// of our hooks that keeps exceptions from getting sucked back into the caller.
// This is useful because we don't want to violate the Vulkan ABI by accident in
// the case that we don't use try/catch somewhere. It's also useful because we
// only use exceptions in unrecoverable absolute failure cases. This means that
// we can just write our code while ignoring the potential for it to throw and
// have errors somewhat gracefully handled by this wrapper.
//
// I was considering mapping certain exception types like std::out_of_memory to
// their vulkan equivalent (only when allowed by the API). In the end I think
// it's just bloat and ultimately less informative than a 'VK_ERROR_UNKNOWN'
// because then the caller knows that it probably wasn't triggered as part of
// the standard Vulkan codepath.
template <auto Func> struct HookExceptionWrapper;
template <typename R, typename... Args, R (*Func)(Args...)>
struct HookExceptionWrapper<Func> {
    static R call(Args... args) noexcept {
        try {
            return Func(args...);
        } catch (...) {
            if constexpr (std::is_same_v<R, VkResult>) {
                return VK_ERROR_UNKNOWN;
            }
        }

        std::terminate();
    }
};

#define HOOK_ENTRY(vk_name_literal, fn_sym)                                    \
    {vk_name_literal, reinterpret_cast<PFN_vkVoidFunction>(                    \
                          &HookExceptionWrapper<fn_sym>::call)}

using func_map_t = std::unordered_map<std::string_view, PFN_vkVoidFunction>;
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

    HOOK_ENTRY("vkGetPhysicalDeviceProperties",
               low_latency::GetPhysicalDeviceProperties),
    HOOK_ENTRY("vkGetPhysicalDeviceProperties2KHR",
               low_latency::GetPhysicalDeviceProperties2KHR),
    HOOK_ENTRY("vkGetPhysicalDeviceProperties2",
               low_latency::GetPhysicalDeviceProperties2),

    HOOK_ENTRY("vkGetPhysicalDeviceSurfaceCapabilities2KHR",
               low_latency::GetPhysicalDeviceSurfaceCapabilities2KHR),
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

    HOOK_ENTRY("vkAntiLagUpdateAMD", low_latency::AntiLagUpdateAMD),

    HOOK_ENTRY("vkGetLatencyTimingsNV", low_latency::GetLatencyTimingsNV),
    HOOK_ENTRY("vkLatencySleepNV", low_latency::LatencySleepNV),
    HOOK_ENTRY("vkQueueNotifyOutOfBandNV", low_latency::QueueNotifyOutOfBandNV),
    HOOK_ENTRY("vkSetLatencyMarkerNV", low_latency::SetLatencyMarkerNV),
    HOOK_ENTRY("vkSetLatencySleepModeNV", low_latency::SetLatencySleepModeNV),

    HOOK_ENTRY("vkCreateSwapchainKHR", low_latency::CreateSwapchainKHR),
    HOOK_ENTRY("vkDestroySwapchainKHR", low_latency::DestroySwapchainKHR),
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

    const auto context = low_latency::layer_context.get_context(device);
    return context->vtable.GetDeviceProcAddr(device, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
LowLatency_GetInstanceProcAddr(VkInstance instance, const char* const pName) {
    if (const auto it = instance_functions.find(pName);
        it != std::end(instance_functions)) {

        return it->second;
    }

    const auto context = low_latency::layer_context.get_context(instance);
    return context->vtable.GetInstanceProcAddr(instance, pName);
}
