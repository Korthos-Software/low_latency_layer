
#ifndef SWAPCHAIN_MONITOR_HH_
#define SWAPCHAIN_MONITOR_HH_

#include "atomic_time_point.hh"
#include "frame_span.hh"
#include "semaphore_signal.hh"

#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace low_latency {

class DeviceContext;

class SwapchainMonitor final {
  private:
    std::vector<std::unique_ptr<FrameSpan>> pending_frame_spans{};

    struct PendingSignal {
        SemaphoreSignal semaphore_signal;
        std::vector<std::unique_ptr<FrameSpan>> frame_spans{};
    };
    std::deque<PendingSignal> pending_signals{};

  protected:
    const DeviceContext& device;

    std::mutex mutex{};
    std::chrono::microseconds present_delay{};
    bool was_low_latency_requested{};
    std::atomic<bool> is_monitor_processing{};
    AtomicTimePoint last_signal_time{};

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

    void notify_semaphore(const SemaphoreSignal& semaphore_signal);

    void attach_work(std::vector<std::unique_ptr<FrameSpan>> submissions);
};

} // namespace low_latency

#endif