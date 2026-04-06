#include "swapchain_monitor.hh"
#include "device_context.hh"
#include "helper.hh"

namespace low_latency {

void SwapchainMonitor::WakeupSemaphore::signal(
    const DeviceContext& device) const {

    const auto ssi =
        VkSemaphoreSignalInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                              .semaphore = this->timeline_semaphore,
                              .value = this->value};
    THROW_NOT_VKSUCCESS(device.vtable.SignalSemaphore(device.device, &ssi));
}

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
                      [&]() { return this->semaphore_submission.has_value(); });

        // Stop only if we're stopped and we have nothing to signal.
        if (stoken.stop_requested() &&
            !this->semaphore_submission.has_value()) {
            break;
        }

        // Grab the most recent semaphore. When work completes, signal it.
        const auto semaphore_submission =
            std::move(*this->semaphore_submission);
        this->semaphore_submission.reset();

        // If we're stopping, signal the semaphore and don't worry about work
        // actually completing.
        if (stoken.stop_requested()) {
            semaphore_submission.wakeup_semaphore.signal(this->device);
            break;
        }

        // Unlock, wait for work to finish, signal semaphore.
        lock.unlock();
        // Ugly and duplicated - will fix this soon.
        if (!semaphore_submission.submissions->empty()) {
            semaphore_submission.submissions->back().end->await_time();
        }

        // TODO add wait for frame pacing
        semaphore_submission.wakeup_semaphore.signal(this->device);
    }
}

void SwapchainMonitor::notify_semaphore(const VkSemaphore& timeline_semaphore,
                                        const std::uint64_t& value) {

    auto lock = std::unique_lock{this->mutex};

    const auto wakeup_semaphore = WakeupSemaphore{
        .timeline_semaphore = timeline_semaphore, .value = value};

    // Signal immediately if reflex is off or it's a no-op submit.
    if (!this->was_low_latency_requested) {
        wakeup_semaphore.signal(this->device);
        return;
    }

    // Signal immediately if we have no outstanding work.
    if (!this->pending_submissions) {
        this->pending_submissions.reset();
        wakeup_semaphore.signal(this->device);
        return;
    }

    this->semaphore_submission.emplace(SemaphoreSubmissions{
        .wakeup_semaphore = wakeup_semaphore,
        .submissions = std::move(this->pending_submissions),
    });
    this->pending_submissions.reset();

    lock.unlock();
    this->cv.notify_one();
}

void SwapchainMonitor::attach_work(
    std::unique_ptr<std::deque<Submission>> submissions) {

    const auto lock = std::scoped_lock{this->mutex};
    if (!this->was_low_latency_requested) {
        return;
    }

    this->pending_submissions = std::move(submissions);
}

} // namespace low_latency