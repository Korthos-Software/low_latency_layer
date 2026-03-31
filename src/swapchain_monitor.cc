#include "swapchain_monitor.hh"
#include "device_context.hh"
#include "helper.hh"

#include <vulkan/vulkan_core.h>

#include <functional>
#include <mutex>

namespace low_latency {

SwapchainMonitor::SwapchainMonitor(const DeviceContext& device,
                                   const bool was_low_latency_requested)
    : device(device), was_low_latency_requested(was_low_latency_requested) {}

SwapchainMonitor::~SwapchainMonitor() {}

void SwapchainMonitor::update_params(
    const bool was_low_latency_requested,
    const std::chrono::milliseconds present_delay) {

    this->was_low_latency_requested = was_low_latency_requested;
    this->present_delay = present_delay;
}

void SwapchainMonitor::prune_submissions() {
    // If our submissions grow too large, we should delete them from our
    // tracking. It would be nice if this was handled elegantly by some custom
    // container and we didn't have to call this manually each time we insert.
    // Also this exact logic is repeated in QueueContext's Submission.
    if (std::size(this->in_flight_submissions) >
        this->MAX_TRACKED_IN_FLIGHT_SUBMISSIONS) {

        this->in_flight_submissions.pop_front();
    }
}

ReflexSwapchainMonitor::ReflexSwapchainMonitor(
    const DeviceContext& device, const bool was_low_latency_requested)
    : SwapchainMonitor(device, was_low_latency_requested),
      monitor_worker(
          std::bind_front(&ReflexSwapchainMonitor::do_monitor, this)) {}

ReflexSwapchainMonitor::~ReflexSwapchainMonitor() {}

void ReflexSwapchainMonitor::WakeupSemaphore::signal(
    const DeviceContext& device) const {

    const auto ssi =
        VkSemaphoreSignalInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                              .semaphore = this->timeline_semaphore,
                              .value = this->value};
    THROW_NOT_VKSUCCESS(device.vtable.SignalSemaphore(device.device, &ssi));
}

void ReflexSwapchainMonitor::do_monitor(const std::stop_token stoken) {
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

            this->in_flight_submissions.back()->await_completed();
            this->in_flight_submissions.clear();
        }

        // We might want to signal them all? In theory it's the same timeline
        // semaphore so obviously it's redundant to signal them one by one. In
        // almost all cases, there should just be one here anyway.
        const auto wakeup_semaphore = this->wakeup_semaphores.back();
        wakeup_semaphores.clear();

        wakeup_semaphore.signal(this->device);
    }
}

void ReflexSwapchainMonitor::notify_semaphore(
    const VkSemaphore& timeline_semaphore, const std::uint64_t& value) {

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

void ReflexSwapchainMonitor::notify_present(
    std::unique_ptr<QueueContext::Submissions> submissions) {

    const auto lock = std::scoped_lock{this->mutex};

    if (!this->was_low_latency_requested) {
        return;
    }

    // Fast path where this work has already completed.
    // In this case, don't wake up the thread. We can just signal
    // what we have immediately on this thread.
    if (!this->wakeup_semaphores.empty() && submissions->has_completed()) {
        this->wakeup_semaphores.back().signal(this->device);
        this->wakeup_semaphores.clear();
        return;
    }

    this->in_flight_submissions.emplace_back(std::move(submissions));
    this->prune_submissions();

    this->cv.notify_one();
}

AntiLagSwapchainMonitor::AntiLagSwapchainMonitor(
    const DeviceContext& device, const bool was_low_latency_requested)
    : SwapchainMonitor(device, was_low_latency_requested) {}

AntiLagSwapchainMonitor::~AntiLagSwapchainMonitor() {}
void AntiLagSwapchainMonitor::notify_present(
    std::unique_ptr<QueueContext::Submissions> submissions) {

    if (!this->was_low_latency_requested) {
        return;
    }

    this->in_flight_submissions.emplace_back(std::move(submissions));
    this->prune_submissions();
}

void AntiLagSwapchainMonitor::await_submissions() {
    if (this->in_flight_submissions.empty()) {
        return;
    }

    this->in_flight_submissions.back()->await_completed();
    this->in_flight_submissions.clear();
}

} // namespace low_latency