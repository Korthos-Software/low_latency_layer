#include "swapchain_monitor.hh"
#include "device_context.hh"
#include "instance_context.hh"
#include "layer_context.hh"

#include <functional>

namespace low_latency {

SwapchainMonitor::SwapchainMonitor(const DeviceContext& device,
                                   FramegenPacer& framegen_pacer)
    : device(device), framegen_pacer(framegen_pacer), delay_controller(device.instance.is_simulation_decoupled),
      monitor_worker(std::bind_front(&SwapchainMonitor::do_monitor, this)) {}

SwapchainMonitor::~SwapchainMonitor() {}

void SwapchainMonitor::update_params(const bool was_low_latency_requested,
                                     const std::chrono::microseconds delay) {

    const auto lock = std::scoped_lock{this->mutex};

    this->was_low_latency_requested = was_low_latency_requested;
    this->present_delay = delay;
}

void SwapchainMonitor::do_monitor(const std::stop_token stoken) {
    for (;;) {
        auto lock = std::unique_lock{this->mutex};
        this->cv.wait(lock, stoken,
                      [&]() { return !this->pending_signals.empty(); });

        // Stop only if we're stopped and we have nothing to signal.
        if (stoken.stop_requested() && this->pending_signals.empty()) {
            break;
        }

        // Grab the most recent semaphore. When work completes, signal it.
        const auto pending_signal = std::move(this->pending_signals.front());
        this->pending_signals.pop_front();

        // If we're stopping, signal the semaphore and don't worry about work
        // actually completing. But we MUST drain them, or we get a hang.
        if (stoken.stop_requested()) {
            pending_signal.semaphore_signal.signal(this->device);
            continue;
        }

        // Grab mutex protected present delay before we sleep - doesn't matter
        // if it's 'old'.
        const auto delay = this->present_delay;
        lock.unlock();

        // Wait for work to complete.
        for (const auto& submission_span : pending_signal.submission_spans) {
            if (submission_span) {
                submission_span->await_completed();
            }
        }

        if (this->device.instance.layer.should_pace_framegen) {
            this->framegen_pacer.hold(pending_signal.sleep_called);
        }

        // Don't need to worry about locking for delay_controller as it's only
        // accessed here.
        this->delay_controller.delay(delay);

        pending_signal.semaphore_signal.signal(this->device);
    }
}

void SwapchainMonitor::notify_semaphore(
    const SemaphoreSignal& semaphore_signal) {

    auto lock = std::unique_lock{this->mutex};

    // Signal immediately if reflex is off or it's a no-op submit.
    if (!this->was_low_latency_requested) {
        semaphore_signal.signal(this->device);
        return;
    }

    this->pending_signals.emplace_back(PendingSignal{
        .semaphore_signal = semaphore_signal,
        .submission_spans = std::move(this->pending_submission_spans),
        .sleep_called = DeviceClock::now(),
    });
    this->pending_submission_spans.clear();

    lock.unlock();
    this->cv.notify_one();
}

void SwapchainMonitor::attach_work(
    std::vector<std::unique_ptr<SubmissionSpan>> submission_spans) {

    const auto lock = std::scoped_lock{this->mutex};
    if (!this->was_low_latency_requested) {
        return;
    }
    this->pending_submission_spans = std::move(submission_spans);
}

} // namespace low_latency