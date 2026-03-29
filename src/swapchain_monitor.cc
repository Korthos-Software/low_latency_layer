#include "swapchain_monitor.hh"
#include "device_context.hh"

#include <vulkan/vulkan_core.h>

#include <functional>
#include <mutex>

namespace low_latency {

SwapchainMonitor::SwapchainMonitor(const DeviceContext& device,
                                   const bool was_low_latency_requested)
    : device(device), was_low_latency_requested(was_low_latency_requested),
      swapchain_worker(
          std::bind_front(&SwapchainMonitor::do_swapchain_monitor, this)) {}

SwapchainMonitor::~SwapchainMonitor() {}

void SwapchainMonitor::WakeupSemaphore::signal(
    const DeviceContext& device) const {

    const auto ssi =
        VkSemaphoreSignalInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                              .semaphore = this->timeline_semaphore,
                              .value = this->value};
    THROW_NON_VKSUCCESS(device.vtable.SignalSemaphore(device.device, &ssi));
}

void SwapchainMonitor::do_swapchain_monitor(const std::stop_token stoken) {
    for (;;) {
        auto lock = std::unique_lock{this->mutex};
        this->cv.wait(lock, stoken,
                      [&]() { return !this->wakeup_semaphores.empty(); });

        if (stoken.stop_requested()) {
            // Small chance an application might need outstanding semaphores
            // to be signalled if it's closing to avoid a hang.
            break;
        }

        // Look for the latest submission and make sure it's completed.
        if (!this->in_flight_submissions.empty()) {
            const auto submission = this->in_flight_submissions.back();
            this->in_flight_submissions.clear();

            if (!submission->empty()) {
                submission->back()->tail_handle->await_time();
            }
        }

        // We might want to signal them all? In theory it's the same timeline
        // semaphore so obviously it's redundant to signal them one by one. In
        // almost all cases, there should just be one here anyway.
        const auto wakeup_semaphore = this->wakeup_semaphores.back();
        wakeup_semaphores.clear();

        wakeup_semaphore.signal(this->device);
    }
}

void SwapchainMonitor::update_params(
    const bool was_low_latency_requested,
    const std::chrono::milliseconds present_delay) {

    const auto lock = std::scoped_lock{this->mutex};

    this->was_low_latency_requested = was_low_latency_requested;
    this->present_delay = present_delay;
}

void SwapchainMonitor::notify_semaphore(const VkSemaphore& timeline_semaphore,
                                        const std::uint64_t& value) {

    const auto lock = std::scoped_lock{this->mutex};

    const auto wakeup_semaphore = WakeupSemaphore{
        .timeline_semaphore = timeline_semaphore, .value = value};

    // Signal immediately if low_latency isn't requested or if we have no
    // outstanding work.
    if (!this->was_low_latency_requested ||
        this->in_flight_submissions.empty()) {

        wakeup_semaphore.signal(this->device);
        return;
    }

    this->wakeup_semaphores.emplace_back(timeline_semaphore, value);
    this->cv.notify_one();
}

void SwapchainMonitor::notify_present(
    const QueueContext::submissions_t& submissions) {

    const auto lock = std::scoped_lock{this->mutex};

    // Fast path where this work has already completed.
    if (!this->wakeup_semaphores.empty() && !submissions->empty()) {

        const auto& finished = submissions->back()->tail_handle->get_time();
        if (finished.has_value()) {
            this->wakeup_semaphores.back().signal(this->device);
            this->wakeup_semaphores.clear();
            return;
        }
    }

    this->in_flight_submissions.emplace_back(submissions);
    this->cv.notify_one();
}

} // namespace low_latency