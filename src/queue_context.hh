#ifndef QUEUE_STATE_HH_
#define QUEUE_STATE_HH_

#include "timestamp_pool.hh"

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan.hpp>

#include <memory>

namespace low_latency {
  
class DeviceContext;

class QueueContext final {
  public:
    DeviceContext& device_context;

    const VkQueue queue;
    const std::uint32_t queue_family_index;

    VkSemaphore semaphore;
    VkCommandPool command_pool;

    std::unique_ptr<TimestampPool> timestamp_pool;

  public:
    QueueContext(DeviceContext& device_context, const VkQueue& queue,
                 const std::uint32_t& queue_family_index);
    QueueContext(const QueueContext&) = delete;
    QueueContext(QueueContext&&) = delete;
    QueueContext operator==(const QueueContext&) = delete;
    QueueContext operator==(QueueContext&&) = delete;
    ~QueueContext();
};

}; // namespace low_latency

#endif