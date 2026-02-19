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

    VkCommandPool command_pool;

    std::unique_ptr<TimestampPool> timestamp_pool;

  private:
    static constexpr auto MAX_TRACKED_TIMINGS = 50;

    // Potentially in flight queue submissions that come from this queue.
    struct Submission {
        const std::unordered_set<VkSemaphore> signals;
        const std::unordered_set<VkSemaphore> waits;

        const std::shared_ptr<TimestampPool::Handle> start_handle;
        const std::shared_ptr<TimestampPool::Handle> end_handle;
        
        const DeviceContext::Clock::time_point_t enqueued_time;

        std::string debug;
    };
    using submission_ptr_t = std::shared_ptr<Submission>;
    std::deque<submission_ptr_t> submissions;

    // In flight frame submissions grouped together.
    // The first element in the deque refers to the first submission that
    // contributed to that frame. The last element is the last submission before
    // present was called.
    // std::size(submissions) >= 1 btw
    struct Frame {
        std::deque<submission_ptr_t> submissions;

        // the point that control flow was returned from VkQueuePresentKHR back to the
        // application. 
        DeviceContext::Clock::time_point_t cpu_post_present_time;
    };
    std::deque<Frame> in_flight_frames;

    // Completed frames.
    struct Timing {
        DeviceContext::Clock::time_point_t::duration gputime, cputime;

        Frame frame;
    };
    std::deque<std::unique_ptr<Timing>> timings;

  private:
    void process_frames();

  public:
    QueueContext(DeviceContext& device_context, const VkQueue& queue,
                 const std::uint32_t& queue_family_index);
    virtual ~QueueContext();

  public:
    void notify_submit(const VkSubmitInfo& info,
                       const std::shared_ptr<TimestampPool::Handle> head_handle,
                       const std::shared_ptr<TimestampPool::Handle> tail_handle,
                       const DeviceContext::Clock::time_point_t& now);

    void notify_submit(const VkSubmitInfo2& info,
                       const std::shared_ptr<TimestampPool::Handle> head_handle,
                       const std::shared_ptr<TimestampPool::Handle> tail_handle,
                       const DeviceContext::Clock::time_point_t& now);

    void notify_present(const VkPresentInfoKHR& info);

  public:
    void sleep_in_present();
};

}; // namespace low_latency

#endif