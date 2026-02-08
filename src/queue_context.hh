#ifndef QUEUE_STATE_HH_
#define QUEUE_STATE_HH_

#include "timestamp_pool.hh"

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan.hpp>

#include <deque>
#include <vector>

namespace low_latency {

class QueueContext final {
  public:
    VkDevice device;
    VkuDeviceDispatchTable vtable;

    VkQueue queue;
    std::uint32_t queue_family_index;

    VkSemaphore semaphore;
    VkCommandPool command_pool;

    TimestampPool timestamp_pool;

    std::deque<
        std::vector<std::pair<TimestampPool::Handle, TimestampPool::Handle>>>
        tracked_queues;

  public:
    QueueContext(const VkDevice& device, const VkQueue queue,
               const std::uint32_t& queue_family_index,
               const VkuDeviceDispatchTable& vtable);
    QueueContext(const QueueContext&) = delete;
    QueueContext(QueueContext&&) = delete;
    QueueContext operator==(const QueueContext&) = delete;
    QueueContext operator==(QueueContext&&) = delete;
    ~QueueContext();
};

}; // namespace low_latency

#endif