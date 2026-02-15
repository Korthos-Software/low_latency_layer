#ifndef QUEUE_STATE_HH_
#define QUEUE_STATE_HH_

#include "context.hh"
#include "device_context.hh"
#include "timestamp_pool.hh"

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <unordered_set>

namespace low_latency {

class QueueContext final : public Context {
  public:
    DeviceContext& device_context;

    const VkQueue queue;
    const std::uint32_t queue_family_index;

    // I used to use these to signal when we could read timestamps until
    // I realised you could use hostQueryReset.
    std::uint64_t semaphore_sequence = 0;
    VkSemaphore semaphore;

    VkCommandPool command_pool;

    std::unique_ptr<TimestampPool> timestamp_pool;

  private:
    static constexpr auto MAX_TRACKED_TIMINGS = 50;
    // Potentially in flight queue submissions
    struct Submission {
        const std::unordered_set<VkSemaphore> signals;
        const std::unordered_set<VkSemaphore> waits;

        const std::shared_ptr<TimestampPool::Handle> start_handle;
        const std::shared_ptr<TimestampPool::Handle> end_handle;

        bool end_of_frame_marker = false;
    };
    std::deque<std::shared_ptr<Submission>> submissions;

    // In flight frames!
    // These might come from different contexts.
    struct Frame {

        struct Timepoint {
            const QueueContext& context;
            const std::shared_ptr<TimestampPool::Handle> handle;
        };

        const Timepoint start;
        const Timepoint end;
    };
    std::deque<std::unique_ptr<Frame>> in_flight_frames;

    struct Timing {

        DeviceContext::Clock::time_point_t gpu_start;
        DeviceContext::Clock::time_point_t gpu_end;

        // Distance between the last gpu_end and this one.
        // So one entire go around, including all cpu and gpu.
        DeviceContext::Clock::time_point_t::duration frametime;
    };
    std::deque<std::unique_ptr<Timing>> timings;

  private:
    void process_frames();

  public:
    QueueContext(DeviceContext& device_context, const VkQueue& queue,
                 const std::uint32_t& queue_family_index);
    virtual ~QueueContext();

  public:
    void
    notify_submit(const VkSubmitInfo& info,
                  const std::shared_ptr<TimestampPool::Handle> head_handle,
                  const std::shared_ptr<TimestampPool::Handle> tail_handle);

    void
    notify_submit(const VkSubmitInfo2& info,
                  const std::shared_ptr<TimestampPool::Handle> head_handle,
                  const std::shared_ptr<TimestampPool::Handle> tail_handle);

    void notify_present(const VkPresentInfoKHR& info);

  public:
    std::optional<DeviceContext::Clock::time_point_t> get_sleep_until();
};

}; // namespace low_latency

#endif