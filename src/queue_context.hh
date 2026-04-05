#ifndef QUEUE_STATE_HH_
#define QUEUE_STATE_HH_

#include "context.hh"
#include "device_clock.hh"
#include "strategies/queue_strategy.hh"
#include "timestamp_pool.hh"

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan.hpp>

#include <memory>

namespace low_latency {

class QueueContext final : public Context {
  private:
    static constexpr auto MAX_TRACKED_PRESENT_IDS = 50u;

  public:
    DeviceContext& device;

    const VkQueue queue;
    const std::uint32_t queue_family_index;

    struct CommandPoolOwner final {
      private:
        const QueueContext& queue;
        VkCommandPool command_pool;

      public:
        CommandPoolOwner(const QueueContext& queue);
        CommandPoolOwner(const CommandPoolOwner&) = delete;
        CommandPoolOwner(CommandPoolOwner&&) = delete;
        CommandPoolOwner operator=(const CommandPoolOwner&) = delete;
        CommandPoolOwner operator=(CommandPoolOwner&&) = delete;
        ~CommandPoolOwner();

      public:
        operator const VkCommandPool&() const { return this->command_pool; }
    };
    const std::unique_ptr<CommandPoolOwner> command_pool;

    std::unique_ptr<TimestampPool> timestamp_pool;

    std::unique_ptr<QueueStrategy> strategy;

  public:
    QueueContext(DeviceContext& device_context, const VkQueue& queue,
                 const std::uint32_t& queue_family_index);
    virtual ~QueueContext();

  public:
    bool should_inject_timestamps() const;
};

}; // namespace low_latency

#endif