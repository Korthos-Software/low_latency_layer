#ifndef SWAPCHAIN_MONITOR_HH_
#define SWAPCHAIN_MONITOR_HH_

// The purpose of this file is to provide a SwapchainMonitor

#include <vulkan/vulkan_core.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "queue_context.hh"

namespace low_latency {

class DeviceContext;

// Abstract base class for swapchain completion monitoring. Both implementations
// currently have an option to frame pace, to disable low_latency mode
// (become a no-op), and must track in_flight_submissions to function.
class SwapchainMonitor {
  private:
    static constexpr auto MAX_TRACKED_IN_FLIGHT_SUBMISSIONS = 50u;

  protected:
    const DeviceContext& device;

    std::mutex mutex;

    // Configurarable params for this swapchain.
    std::chrono::microseconds present_delay = std::chrono::microseconds{0};
    bool was_low_latency_requested = false;

    std::deque<std::unique_ptr<QueueContext::Submissions>>
        in_flight_submissions;

  protected:
    // Small fix to avoid submissions growing limitlessly in size if this
    // swapchain is never presented to.
    void prune_submissions();

  public:
    SwapchainMonitor(const DeviceContext& device,
                     const bool was_low_latency_requested);
    SwapchainMonitor(const SwapchainMonitor&) = delete;
    SwapchainMonitor(SwapchainMonitor&&) = delete;
    SwapchainMonitor operator=(const SwapchainMonitor&) = delete;
    SwapchainMonitor operator=(SwapchainMonitor&&) = delete;
    virtual ~SwapchainMonitor();

  public:
    void update_params(const bool was_low_latency_requested,
                       const std::chrono::microseconds present_delay);

  public:
    virtual void
    notify_present(std::unique_ptr<QueueContext::Submissions> submissions) = 0;
};

// Provides asynchronous monitoring of submissions and signalling of some
// timeline semaphore via a worker thread.
class ReflexSwapchainMonitor final : public SwapchainMonitor {
  private:
    struct WakeupSemaphore {
        VkSemaphore timeline_semaphore;
        std::uint64_t value;

      public:
        void signal(const DeviceContext& device) const;
    };

    // A pairing of semaphore -> submissions.
    // If the Submissions completes then signal the bundled semaphore.
    struct SemaphoreSubmissions {
        WakeupSemaphore wakeup_semaphore;
        std::unique_ptr<QueueContext::Submissions> submissions;
    };
    std::deque<SemaphoreSubmissions> semaphore_submissions;

    std::condition_variable_any cv;
    std::jthread monitor_worker;

  private:
    void do_monitor(const std::stop_token stoken);

  public:
    ReflexSwapchainMonitor(const DeviceContext& device,
                           const bool was_low_latency_requested);
    virtual ~ReflexSwapchainMonitor();

  public:
    void notify_semaphore(const VkSemaphore& timeline_semaphore,
                          const std::uint64_t& value);

  public:
    virtual void notify_present(
        std::unique_ptr<QueueContext::Submissions> submissions) override;
};

// Much simpler synchronous waiting without another monitor thread - still need
// to synchronise across threads however.
class AntiLagSwapchainMonitor final : public SwapchainMonitor {
  public:
    AntiLagSwapchainMonitor(const DeviceContext& device,
                            const bool was_low_latency_requested);
    virtual ~AntiLagSwapchainMonitor();

  public:
    // Synchronously wait until all in-flight submissions have completed.
    void await_submissions();

  public:
    virtual void notify_present(
        std::unique_ptr<QueueContext::Submissions> submissions) override;
};

} // namespace low_latency

#endif