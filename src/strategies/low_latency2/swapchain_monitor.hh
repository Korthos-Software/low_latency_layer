
#ifndef SWAPCHAIN_MONITOR_HH_
#define SWAPCHAIN_MONITOR_HH_

#include "delay_controller.hh"
#include "framegen_pacer.hh"
#include "semaphore_signal.hh"
#include "submission_span.hh"

#include <vulkan/vulkan.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace low_latency {

class DeviceContext;

class SwapchainMonitor final {
  private:
    std::vector<std::unique_ptr<SubmissionSpan>> pending_submission_spans{};

    struct PendingSignal final {
        SemaphoreSignal semaphore_signal;
        std::vector<std::unique_ptr<SubmissionSpan>> submission_spans{};
        DeviceClock::time_point sleep_called{};
    };
    std::deque<PendingSignal> pending_signals{};

    const DeviceContext& device;
    FramegenPacer& framegen_pacer;

    DelayController delay_controller;

    std::mutex mutex{};
    std::chrono::microseconds present_delay{};
    bool was_low_latency_requested{};

    std::condition_variable_any cv{};
    std::jthread monitor_worker{};

    void do_monitor(const std::stop_token stoken);

  public:
    explicit SwapchainMonitor(const DeviceContext& device,
                              FramegenPacer& framegen_pacer);
    SwapchainMonitor(const SwapchainMonitor&) = delete;
    SwapchainMonitor(SwapchainMonitor&&) = delete;
    SwapchainMonitor& operator=(const SwapchainMonitor&) = delete;
    SwapchainMonitor& operator=(SwapchainMonitor&&) = delete;
    ~SwapchainMonitor();

  public:
    void update_params(const bool was_low_latency_requested,
                       const std::chrono::microseconds delay);

    void notify_semaphore(const SemaphoreSignal& semaphore_signal);

    void attach_work(std::vector<std::unique_ptr<SubmissionSpan>> submissions);
};

} // namespace low_latency

#endif