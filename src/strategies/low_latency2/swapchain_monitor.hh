
#ifndef SWAPCHAIN_MONITOR_HH_
#define SWAPCHAIN_MONITOR_HH_

#include <vulkan/vulkan.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "submission.hh"

namespace low_latency {

class DeviceContext;

class SwapchainMonitor final {
  private:
    struct WakeupSemaphore {
        VkSemaphore timeline_semaphore{};
        std::uint64_t value{};

      public:
        void signal(const DeviceContext& device) const;
    };

    std::unique_ptr<std::deque<Submission>> pending_submissions{};

    // A pairing of semaphore -> submissions.
    // If the Submissions completes then signal the bundled semaphore.
    struct SemaphoreSubmissions {
        WakeupSemaphore wakeup_semaphore{};
        std::unique_ptr<std::deque<Submission>> submissions{};
    };
    std::optional<SemaphoreSubmissions> semaphore_submission{};

  protected:
    const DeviceContext& device;

    std::mutex mutex{};
    std::chrono::microseconds present_delay{};
    bool was_low_latency_requested{};

    std::condition_variable_any cv{};
    std::jthread monitor_worker{};

    void do_monitor(const std::stop_token stoken);

  public:
    SwapchainMonitor(const DeviceContext& device);
    SwapchainMonitor(const SwapchainMonitor&) = delete;
    SwapchainMonitor(SwapchainMonitor&&) = delete;
    SwapchainMonitor operator=(const SwapchainMonitor&) = delete;
    SwapchainMonitor operator=(SwapchainMonitor&&) = delete;
    ~SwapchainMonitor();

  public:
    void update_params(const bool was_low_latency_requested,
                       const std::chrono::microseconds delay);

    void notify_semaphore(const VkSemaphore& timeline_semaphore,
                          const std::uint64_t& value);

    void attach_work(std::unique_ptr<std::deque<Submission>> submissions);
};

} // namespace low_latency

#endif