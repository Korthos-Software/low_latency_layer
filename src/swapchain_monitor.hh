#ifndef SWAPCHAIN_MONITOR_HH_
#define SWAPCHAIN_MONITOR_HH_

// The purpose of this file is to provide a SwapchainMonitor class definition.

#include <vulkan/vulkan_core.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "queue_context.hh"

namespace low_latency {

class DeviceContext;

// A swapchain monitor's job is to provide asynchronous wakeups for threads
// which request low_latency once the previous presentation has completed.
// It does this by signalling a semaphore a la VK_NV_low_latency2.
class SwapchainMonitor {
  private:
    const DeviceContext& device;

    // Configurarable params for this swapchain.
    std::chrono::milliseconds present_delay = std::chrono::milliseconds{0};
    bool was_low_latency_requested = false;

    struct WakeupSemaphore {
        VkSemaphore timeline_semaphore;
        std::uint64_t value;

      public:
        void signal(const DeviceContext& device) const;
    };
    std::deque<WakeupSemaphore> wakeup_semaphores;
    std::deque<QueueContext::submissions_t> in_flight_submissions;

    std::mutex mutex;
    std::condition_variable_any cv;
    std::jthread swapchain_worker;

  private:
    void do_swapchain_monitor(const std::stop_token stoken);

  public:
    SwapchainMonitor(const DeviceContext& device,
                     const bool was_low_latency_requested);
    SwapchainMonitor(const SwapchainMonitor&);
    SwapchainMonitor(SwapchainMonitor&&);
    SwapchainMonitor operator=(const SwapchainMonitor&);
    SwapchainMonitor operator=(SwapchainMonitor&&);
    ~SwapchainMonitor();

  public:
    void update_params(const bool was_low_latency_requested,
                       const std::chrono::milliseconds present_delay);

    void notify_semaphore(const VkSemaphore& timeline_semaphore,
                          const std::uint64_t& value);

    void notify_present(const QueueContext::submissions_t& submissions);
};

} // namespace low_latency

#endif