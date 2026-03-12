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
  private:
    // The amount of finished frame timing data we keep before eviction.
    // For now, this value is also the number of data points used in the
    // calculation of gpu timing information.
    static constexpr auto MAX_TRACKED_TIMINGS = 50u;
    // The amount of queue submissions we allow tracked per queue before
    // we give up tracking them. For a queue that is presented to,
    // these submissions will be constantly moved to Frame structs so
    // it's not an issue that we only track so many - unless it just
    // happens that an application makes an unexpectedly large
    // amount of vkQueueSubmit's per frame. For queues which don't
    // present, this limit stops them from growing limitlessly in memory
    // as we may not necessarily manually evict them yet.
    static constexpr auto MAX_TRACKED_SUBMISSIONS = 50u;

  public:
    DeviceContext& device_context;

    const VkQueue queue;
    const std::uint32_t queue_family_index;

    struct CommandPoolOwner final {
      private:
        const QueueContext& queue_context;
        VkCommandPool command_pool;

      public:
        CommandPoolOwner(const QueueContext& queue_context);
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

  public:
    // Potentially in flight queue submissions that come from this queue.
    struct Submission {
        const std::unordered_set<VkSemaphore> signals;
        const std::unordered_set<VkSemaphore> waits;

        const std::shared_ptr<TimestampPool::Handle> start_handle;
        const std::shared_ptr<TimestampPool::Handle> end_handle;

        const DeviceContext::Clock::time_point_t enqueued_time;
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

        // the point that control flow was returned from VkQueuePresentKHR back
        // to the application.
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
    // Drains submissions and promotes them into a single frame object.
    void drain_submissions_to_frame();

    // Drains in flight frames and promotes them into a Timing object if they
    // have completed.
    void drain_frames_to_timings();

    // Antilag 1 equivalent where we sleep after present to reduce queueing.
    void sleep_in_present();

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
    bool should_inject_timestamps() const;
};

}; // namespace low_latency

#endif