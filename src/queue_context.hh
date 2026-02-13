#ifndef QUEUE_STATE_HH_
#define QUEUE_STATE_HH_

#include "context.hh"
#include "timestamp_pool.hh"

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <span>
#include <unordered_set>

namespace low_latency {

class DeviceContext;

class QueueContext final : public Context {
  public:
    DeviceContext& device_context;

    const VkQueue queue;
    const std::uint32_t queue_family_index;

    std::uint64_t semaphore_sequence = 0;
    VkSemaphore semaphore;

    VkCommandPool command_pool;

    std::unique_ptr<TimestampPool> timestamp_pool;

    // Potentially in flight queue submissions
    struct Submission {
        const std::unordered_set<VkSemaphore> signals;
        const std::unordered_set<VkSemaphore> waits;
        const std::uint64_t target_semaphore_sequence;
        const std::shared_ptr<TimestampPool::Handle> timestamp_handle;
    };
    std::deque<std::shared_ptr<Submission>> submissions;

    // In flight frames!
    // These might come from different contexts.
    struct Frame {
        const QueueContext& start_context;
        const std::shared_ptr<TimestampPool::Handle> start;
        const std::uint64_t target_start_sequence;

        const QueueContext& end_context;
        const std::shared_ptr<TimestampPool::Handle> end;
        const std::uint64_t target_end_sequence;
    };
    // These can be null, it means we made presented without finding the
    // timestamps associated with the present.
    std::deque<std::unique_ptr<Frame>> in_flight_frames;

  public:
    QueueContext(DeviceContext& device_context, const VkQueue& queue,
                 const std::uint32_t& queue_family_index);
    virtual ~QueueContext();

  public:
    void notify_submit(std::span<const VkSubmitInfo> infos,
                       const std::uint64_t target_semaphore_sequence,
                       std::shared_ptr<TimestampPool::Handle>&& handle);
    void notify_submit(std::span<const VkSubmitInfo2> infos,
                       const std::uint64_t target_semaphore_sequence,
                       std::shared_ptr<TimestampPool::Handle>&& handle);

    void notify_present(const VkPresentInfoKHR& info);

  public:
    // Computes the amount we should delay...
    using duration_t = std::chrono::steady_clock::duration;
    std::optional<duration_t> get_delay_time();
};

}; // namespace low_latency

#endif