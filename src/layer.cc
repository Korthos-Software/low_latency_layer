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

#include "device_context.hh"
#include "instance_context.hh"
#include "layer_context.hh"
#include "queue_context.hh"
#include "timestamp_pool.hh"

namespace low_latency {

namespace {

LayerContext layer_context;

} // namespace

// Small templates which allow us to SFINAE find pNext structs.
template <typename T>
static T* find_next(void* const head, const VkStructureType& stype) {
    for (auto i = reinterpret_cast<VkBaseOutStructure*>(head)->pNext; i;
         i = i->pNext) {

        if (i->sType == stype) {
            return reinterpret_cast<T*>(i);
        }
    }
    return nullptr;
}

template <typename T>
static const T* find_next(const void* const head,
                          const VkStructureType& stype) {

    for (auto i = reinterpret_cast<const VkBaseInStructure*>(head)->pNext; i;
         i = i->pNext) {

        if (i->sType == stype) {
            return reinterpret_cast<const T*>(i);
        }
    }
    return nullptr;
}

template <typename T>
static const T* find_link(const void* const head,
                          const VkStructureType& stype) {
    for (auto info = find_next<T>(head, stype); info;
         info = find_next<T>(info, stype)) {

        if (info->function == VK_LAYER_LINK_INFO) {
            return reinterpret_cast<const T*>(info);
        }
    }
    return nullptr;
}

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

    const auto enabled_extensions =
        std::span{pCreateInfo->ppEnabledExtensionNames,
                  pCreateInfo->enabledExtensionCount};

    const auto requested = std::unordered_set<std::string_view>(
        std::from_range, enabled_extensions);

    // There's the antilag extension that might be requested here - Antilag2.
    // Then there's the other thing we provide, which is our AntiLag1
    // equivalent. Calling them AL1 and AL2, where AL1 is requested via
    // an env var and AL2 is requested at the device level via the extension,
    // the cases where we exit with a bad code or deliberately no-op are:
    //
    //     !SUPPORTED && !AL2 &&  AL1          -> No-op hooks
    //                   !AL2 && !AL1          -> No-op hooks.
    //     !SUPPORTED &&  AL2                  -> VK_ERROR_INITIALIZATION_FAILED
    //
    // Note that even though the user has explicitly enabled AL1 via an env var,
    // failing hard here by returning INIT_FAILED if the device doesn't support
    // it is wrong. The vulkan application could just be creating a device that
    // cannot support it which is unrelated to anything present related. This
    // is not the case with AL2, because the vulkan application has to
    // explicitly ask for the extension when it creates the device.

    const auto was_antilag_requested =
        requested.contains(VK_AMD_ANTI_LAG_EXTENSION_NAME) ||
        requested.contains(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);

    const auto context = layer_context.get_context(physical_device);
    if (!context->supports_required_extensions && was_antilag_requested) {
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

        // Don't append anything extra if we don't support what we need.
        if (!context->supports_required_extensions) {
            return next_extensions;
        }

        // Only append the extra extension if it wasn't already asked for.
        for (const auto& wanted : PhysicalDeviceContext::required_extensions) {
            if (requested.contains(wanted)) {
                continue;
            }

            next_extensions.push_back(wanted);
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
#undef DEVICE_VTABLE_LOAD

    const auto key = layer_context.get_key(*pDevice);
    const auto lock = std::scoped_lock{layer_context.mutex};

    assert(!layer_context.contexts.contains(key));
    layer_context.contexts.try_emplace(
        key, std::make_shared<DeviceContext>(context->instance, *context,
                                             *pDevice, was_antilag_requested,
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
    const auto& vtable = context->device_context.vtable;

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

    const auto now = DeviceContext::Clock::now();

    std::ranges::transform(
        std::span{submit_infos, submit_count}, std::back_inserter(next_submits),
        [&](const auto& submit) {
            const auto head_handle = context->timestamp_pool->acquire();
            const auto tail_handle = context->timestamp_pool->acquire();
            head_handle->setup_command_buffers(*tail_handle, *context);
            context->notify_submit(submit, head_handle, tail_handle, now);

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
    const auto& vtable = context->device_context.vtable;

    if (!submit_count || !context->should_inject_timestamps()) {
        return vtable.QueueSubmit2(queue, submit_count, submit_infos, fence);
    }

    using cbs_t = std::vector<VkCommandBufferSubmitInfo>;
    auto next_submits = std::vector<VkSubmitInfo2>{};
    auto next_cbs = std::vector<std::unique_ptr<cbs_t>>{};
    auto handles = std::vector<std::shared_ptr<TimestampPool::Handle>>{};

    const auto now = DeviceContext::Clock::now();

    std::ranges::transform(
        std::span{submit_infos, submit_count}, std::back_inserter(next_submits),
        [&](const auto& submit) {
            const auto head_handle = context->timestamp_pool->acquire();
            const auto tail_handle = context->timestamp_pool->acquire();
            head_handle->setup_command_buffers(*tail_handle, *context);
            context->notify_submit(submit, head_handle, tail_handle, now);

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
    const auto& vtable = context->device_context.vtable;

    if (const auto res = vtable.QueuePresentKHR(queue, present_info);
        res != VK_SUCCESS) {

        return res;
    }

    context->notify_present(*present_info);

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device, const char* pLayerName,
    std::uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {

    const auto context = layer_context.get_context(physical_device);
    const auto& vtable = context->instance.vtable;

    // Not asking about our layer - just forward it.
    if (!pLayerName || std::string_view{pLayerName} != LAYER_NAME) {
        return vtable.EnumerateDeviceExtensionProperties(
            physical_device, pLayerName, pPropertyCount, pProperties);
    }

    auto& count = *pPropertyCount;
    // !pProperties means they're querying how much space they need.
    if (!pProperties) {
        count = 1;
        return VK_SUCCESS;
    }

    if (!count) {
        return VK_INCOMPLETE; // They gave us zero space to work with.
    }

    // If we're spoofing nvidia we want to provide their extension instead.
    const auto extension_properties = [&]() -> VkExtensionProperties {
        if (context->instance.layer.should_spoof_nvidia) {
            return {.extensionName = VK_NV_LOW_LATENCY_2_EXTENSION_NAME,
                    .specVersion = VK_NV_LOW_LATENCY_2_SPEC_VERSION};
        }
        return {.extensionName = VK_AMD_ANTI_LAG_EXTENSION_NAME,
                .specVersion = VK_AMD_ANTI_LAG_SPEC_VERSION};
    }();

    pProperties[0] = extension_properties;
    count = 1;

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2* pFeatures) {

    const auto context = layer_context.get_context(physical_device);
    const auto& vtable = context->instance.vtable;

    vtable.GetPhysicalDeviceFeatures2(physical_device, pFeatures);

    // Don't provide AntiLag if we're trying to spoof nvidia.
    // Nvidia uses VkSurfaceCapabilities2KHR to determine if a surface
    // is capable of reflex instead of AMD's physical device switch found here.
    if (context->instance.layer.should_spoof_nvidia) {
        return;
    }

    const auto feature = find_next<VkPhysicalDeviceAntiLagFeaturesAMD>(
        pFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD);

    if (feature) {
        feature->antiLag = context->supports_required_extensions;
    }
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2KHR(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2KHR* pFeatures) {
    return low_latency::GetPhysicalDeviceFeatures2(physical_device, pFeatures);
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
    if (!context->instance.layer.should_spoof_nvidia) {
        return;
    }

    const auto lsc = find_next<VkLatencySurfaceCapabilitiesNV>(
        pSurfaceCapabilities,
        VK_STRUCTURE_TYPE_LATENCY_SURFACE_CAPABILITIES_NV);

    if (!lsc) {
        return;
    }

    // I kind of eyeballed these!
    const auto supported_modes = std::vector<VkPresentModeKHR>{
        VK_PRESENT_MODE_IMMEDIATE_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_FIFO_KHR,
    };
    const auto num_supported_modes =
        static_cast<std::uint32_t>(std::size(supported_modes));

    // They're asking how many we want to return.
    if (!lsc->pPresentModes) {
        lsc->presentModeCount = static_cast<std::uint32_t>(num_supported_modes);
        return;
    }

    // Finally we can write what surfaces are capable.
    const auto num_to_write =
        std::min(lsc->presentModeCount, num_supported_modes);

    std::ranges::copy_n(std::begin(supported_modes), num_to_write,
                        lsc->pPresentModes);

    lsc->presentModeCount = num_to_write;
}

static VKAPI_ATTR void VKAPI_CALL
AntiLagUpdateAMD(VkDevice device, const VkAntiLagDataAMD* pData) {
    const auto context = layer_context.get_context(device);
    assert(pData);
    context->notify_antilag_update(*pData);
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
