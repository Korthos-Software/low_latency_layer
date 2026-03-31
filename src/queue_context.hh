#ifndef QUEUE_STATE_HH_
#define QUEUE_STATE_HH_

#include "context.hh"
#include "device_clock.hh"
#include "timestamp_pool.hh"

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan.hpp>

#include <deque>
#include <memory>
#include <unordered_map>

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

    // NVIDIA's extension lets the application explicitly state that this queue
    // does not contribute to the frame. AMD's extension has no such mechanism -
    // so this will always be false when using VK_AMD_anti_lag.
    bool is_out_of_band = false;

  public:
    // I want our queue bookkeeping to be fairly simple and do one thing - track
    // submissions that have yet to have been presented to a swapchain. General
    // idea:
    //
    // For each vkQueueSubmit (specifically for each pSubmitInfo in that
    // hook) grab the VK_EXT_present_id value provided by the application for
    // that submission. Once we add our timing objects as part of the hook, we
    // then take those timing objects, bundle them into a Submission struct, and
    // append it to the (potentially currently nonexistent) mapping of
    // present_id's to deque<Submission>'s. Now we cleanly track what queue
    // submissions refer to what present_id.
    //
    // When our hook sees a VkQueuePresentKHR, we take the provided present_id
    // and notify our device that it needs to watch for when this completes.
    // We give it our submissions. Now, it's out of our hands. We remove the
    // present_id_t mapping when doing so.

    class Submissions final {
        // The amount of queue submissions we allow tracked per queue before
        // we give up tracking them. This is neccessary for queues which do not
        // present anything.
        static constexpr auto MAX_TRACKED_SUBMISSIONS = 50u;

        struct Submission final {
            std::shared_ptr<TimestampPool::Handle> head_handle, tail_handle;
            DeviceClock::time_point_t cpu_present_time;
        };
        std::deque<std::unique_ptr<Submission>> submissions;

      public:
        Submissions();
        Submissions(const Submissions&) = delete;
        Submissions(Submissions&&) = delete;
        Submissions operator=(const Submissions&) = delete;
        Submissions operator=(Submissions&&) = delete;
        ~Submissions();

      public:
        void add_submission(const std::shared_ptr<TimestampPool::Handle> head,
                            const std::shared_ptr<TimestampPool::Handle> tail,
                            const DeviceClock::time_point_t& now);

        // Non-blocking - true if this submission has completed on the GPU.
        bool has_completed() const;
        // Blocking wait until the last submission has completed.
        void await_completed() const;
    };

    using present_id_t = std::uint64_t;
    using submissions_ptr_t = std::shared_ptr<Submissions>;
    std::unordered_map<present_id_t, submissions_ptr_t> unpresented_submissions;

    // We might be tracking present_ids which aren't presented to - and as a
    // result we don't ever clear those Submissions. So manually evict them by
    // removing the n'th oldest. This is elegant because even if our
    // SwapchainMonitor has these stored (unlikely) they won't be destructed as
    // it just decrements their std::shared_ptr use count.
    std::deque<present_id_t> present_id_ring;

  public:
    QueueContext(DeviceContext& device_context, const VkQueue& queue,
                 const std::uint32_t& queue_family_index);
    virtual ~QueueContext();

  public:
    void notify_submit(const present_id_t& present_id,
                       const std::shared_ptr<TimestampPool::Handle> head_handle,
                       const std::shared_ptr<TimestampPool::Handle> tail_handle,
                       const DeviceClock::time_point_t& now);

    void notify_present(const VkSwapchainKHR& swapchain,
                        const std::uint64_t& present_id);

  public:
    bool should_inject_timestamps() const;
};

}; // namespace low_latency

#endif