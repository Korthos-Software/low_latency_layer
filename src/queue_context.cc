#include "queue_context.hh"
#include "device_context.hh"
#include "helper.hh"
#include "timestamp_pool.hh"

#include <span>

#include <vulkan/vulkan_core.h>

namespace low_latency {

QueueContext::CommandPoolOwner::CommandPoolOwner(const QueueContext& queue)
    : queue(queue) {

    const auto& device_context = this->queue.device;

    const auto cpci = VkCommandPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue.queue_family_index,
    };

    THROW_NOT_VKSUCCESS(device_context.vtable.CreateCommandPool(
        device_context.device, &cpci, nullptr, &this->command_pool));
}

QueueContext::CommandPoolOwner::~CommandPoolOwner() {
    const auto& device_context = this->queue.device;
    device_context.vtable.DestroyCommandPool(device_context.device,
                                             this->command_pool, nullptr);
}

QueueContext::QueueContext(DeviceContext& device, const VkQueue& queue,
                           const std::uint32_t& queue_family_index)
    : device(device), queue(queue), queue_family_index(queue_family_index),
      command_pool(std::make_unique<CommandPoolOwner>(*this)) {

    // Only construct a timestamp pool if we support it!
    if (device.physical_device.supports_required_extensions) {
        this->timestamp_pool = std::make_unique<TimestampPool>(*this);
    }
}

QueueContext::~QueueContext() {
    this->unpresented_submissions.clear();
    this->timestamp_pool.reset();
}

void QueueContext::notify_submit(
    const present_id_t& present_id,
    const std::shared_ptr<TimestampPool::Handle> head_handle,
    const std::shared_ptr<TimestampPool::Handle> tail_handle,
    const DeviceClock::time_point_t& now) {

    // Push this submission onto our unpresented_submissions at our present_id
    // mapping (might be empty, but handled with operator[]).
    auto& submissions = this->unpresented_submissions[present_id];
    if (submissions == nullptr) {
        submissions =
            std::make_shared<std::deque<std::unique_ptr<Submission>>>();

        if (present_id) {
            this->present_id_ring.emplace_back(present_id);
        }
    }

    submissions->push_back(
        std::make_unique<Submission>(Submission{.head_handle = head_handle,
                                                .tail_handle = tail_handle,
                                                .cpu_present_time = now}));

    // This is probably hit if our queue never actually presents to anything.
    if (std::size(*submissions) > this->MAX_TRACKED_SUBMISSIONS) {
        submissions->pop_front();
    }

    if (std::size(this->present_id_ring) > MAX_TRACKED_PRESENT_IDS) {
        const auto evicted_present_id = this->present_id_ring.front();
        this->present_id_ring.pop_front();

        this->unpresented_submissions.erase(evicted_present_id);
    }
}

void QueueContext::notify_present(const VkSwapchainKHR& swapchain,
                                  const present_id_t& present_id) {

    // Notify the device that this swapchain was just presented to.
    // We're avoiding a double hash here - don't use operator[] and erase.
    auto iter = this->unpresented_submissions.try_emplace(present_id).first;
    if (iter->second == nullptr) {
        iter->second =
            std::make_shared<std::deque<std::unique_ptr<Submission>>>();
    }

    this->device.notify_present(swapchain, iter->second);

    // Important, we nuke the submission because now it's presented.
    this->unpresented_submissions.erase(iter);
}

bool QueueContext::should_inject_timestamps() const {
    const auto& physical_device = this->device.physical_device;

    // Our layer is a no-op here if we don't support it.
    if (!physical_device.supports_required_extensions) {
        return false;
    }

    // Don't bother injecting timestamps during queue submission if we
    // aren't planning on doing anything anyway.
    if (!this->device.was_capability_requested) {
        return false;
    }

    // Don't do it if we've been marked as 'out of band' by nvidia's extension.
    if (this->should_ignore_latency) {
        return false;
    }

    assert(physical_device.queue_properties);
    const auto& queue_props = *physical_device.queue_properties;
    assert(this->queue_family_index < std::size(queue_props));

    const auto& props = queue_props[this->queue_family_index];
    // Probably need at least 64, don't worry about it just yet and just ensure
    // it's not zero (because that will cause a crash if we inject).
    return props.queueFamilyProperties.timestampValidBits;
}

} // namespace low_latency