#include "queue_context.hh"
#include "device_context.hh"
#include "timestamp_pool.hh"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <span>

namespace low_latency {

static VkCommandPool
make_command_pool(const DeviceContext& device_context,
                  const std::uint32_t& queue_family_index) {

    const auto cpci = VkCommandPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family_index,
    };

    auto command_pool = VkCommandPool{};
    device_context.vtable.CreateCommandPool(device_context.device, &cpci,
                                            nullptr, &command_pool);
    return command_pool;
}

static VkSemaphore make_semaphore(const DeviceContext& device_context) {

    const auto stci = VkSemaphoreTypeCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };

    const auto sci = VkSemaphoreCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &stci,
    };

    auto semaphore = VkSemaphore{};
    device_context.vtable.CreateSemaphore(device_context.device, &sci, nullptr,
                                          &semaphore);
    return semaphore;
}

QueueContext::QueueContext(DeviceContext& device_context, const VkQueue& queue,
                           const std::uint32_t& queue_family_index)
    : device_context(device_context), queue(queue),
      queue_family_index(queue_family_index),
      // Important we make the command pool before the timestamp pool, because
      // it's a dependency.
      command_pool(make_command_pool(device_context, queue_family_index)),
      semaphore(make_semaphore(device_context)),
      timestamp_pool(std::make_unique<TimestampPool>(*this)) {}

QueueContext::~QueueContext() {

    this->in_flight_frames.clear();
    this->submissions.clear();
    this->timestamp_pool.reset();

    const auto& vtable = this->device_context.vtable;
    vtable.DestroySemaphore(this->device_context.device, this->semaphore,
                            nullptr);
    vtable.DestroyCommandPool(this->device_context.device, this->command_pool,
                              nullptr);
}

void QueueContext::notify_submit(
    const VkSubmitInfo& info,
    const std::shared_ptr<TimestampPool::Handle> head_handle,
    const std::shared_ptr<TimestampPool::Handle> tail_handle) {

    auto signals = std::unordered_set<VkSemaphore>{};
    auto waits = std::unordered_set<VkSemaphore>{};
    std::ranges::copy_n(info.pWaitSemaphores, info.waitSemaphoreCount,
                        std::inserter(waits, std::end(waits)));
    std::ranges::copy_n(info.pSignalSemaphores, info.signalSemaphoreCount,
                        std::inserter(signals, std::end(signals)));

    this->submissions.emplace_back(std::make_unique<Submission>(
        std::move(signals), std::move(waits), head_handle, tail_handle));

    // TODO HACK
    if (std::size(this->submissions) > 100) {
        this->submissions.pop_front();
    }
}

void QueueContext::notify_submit(
    const VkSubmitInfo2& info,
    const std::shared_ptr<TimestampPool::Handle> head_handle,
    const std::shared_ptr<TimestampPool::Handle> tail_handle) {

    auto signals = std::unordered_set<VkSemaphore>{};
    auto waits = std::unordered_set<VkSemaphore>{};

    std::ranges::transform(
        std::span{info.pWaitSemaphoreInfos, info.waitSemaphoreInfoCount},
        std::inserter(waits, std::end(waits)),
        [](const auto& info) -> auto { return info.semaphore; });

    std::ranges::transform(
        std::span{info.pSignalSemaphoreInfos, info.signalSemaphoreInfoCount},
        std::inserter(signals, std::end(signals)),
        [](const auto& info) -> auto { return info.semaphore; });

    std::cerr << "submit2 notif for queue " << this->queue << '\n';
    std::cerr << "    signals: \n";
    for (const auto& signal : signals) {
        std::cerr << "      " << signal << '\n';
    }
    std::cerr << "    waits: \n";
    for (const auto& wait : waits) {
        std::cerr << "      " << wait << '\n';
    }

    this->submissions.emplace_back(std::make_unique<Submission>(
        std::move(signals), std::move(waits), head_handle, tail_handle));

    // TODO HACK
    if (std::size(this->submissions) > 100) {
        this->submissions.pop_front();
    }
}

void QueueContext::notify_present(const VkPresentInfoKHR& info) {

    const auto waits = [&]() {
        auto waits = std::unordered_set<VkSemaphore>{};
        std::ranges::copy_n(info.pWaitSemaphores, info.waitSemaphoreCount,
                            std::inserter(waits, std::end(waits)));
        return waits;
    }();

    const auto collected_semaphores = [&info, this]() {
        auto collected_semaphores = std::unordered_set<VkSemaphore>{};
        for (auto i = std::uint32_t{0}; i < info.swapchainCount; ++i) {
            const auto& swapchain = info.pSwapchains[i];
            const auto& index = info.pImageIndices[i];

            // Shouldn't be possible to present to a swapchain that wasn't
            // waited in

            const auto& signals = this->device_context.swapchain_signals;
            const auto swapchain_it = signals.find(swapchain);
            assert(swapchain_it != std::end(signals));
            const auto index_it = swapchain_it->second.find(index);
            assert(index_it != std::end(swapchain_it->second));

            const auto& semaphore = index_it->second;
            collected_semaphores.emplace(index_it->second);
        }
        return collected_semaphores;
    }();

    const auto acquire_iter = std::ranges::find_if(
        std::rbegin(this->submissions), std::rend(this->submissions),
        [&](const auto& submission) {
            return std::ranges::any_of(
                submission->waits, [&](const auto& wait) {
                    return collected_semaphores.contains(wait);
                });
        });

    if (acquire_iter == std::rend(this->submissions)) {
        std::cerr << "couldn't find starting submission!\n";
        return;
    }
    const auto& acquire = *acquire_iter;

    const auto present_iter = std::ranges::find_if(
        std::rbegin(this->submissions), std::rend(this->submissions),
        [&](const auto& submission) {
            return std::ranges::any_of(
                submission->signals,
                [&](const auto& signal) { return waits.contains(signal); });
        });

    if (present_iter == std::rend(this->submissions)) {
        std::cerr << "couldn't find ending submission!\n";
        return;
    }
    const auto& end = *present_iter;

    std::cerr << "present for queue: " << queue << ", our waits:\n";
    for (const auto& wait : waits) {
        std::cerr << "      " << wait << '\n';
    }

    // The work including and between acquire -> present is effectively
    // guaranteed to contribute to our frame. We are going to mark this point
    // for future queues to read the 'start of frame' from.
    (*present_iter)->end_of_frame_marker = true;

    // Now we read backwards to try to find our true start, starting at our
    // acquire.
    const auto start_iter = std::prev(std::ranges::find_if(
        std::next(acquire_iter), std::rend(this->submissions),
        [](const auto& submission) {
            return submission->end_of_frame_marker;
        }));
    const auto& start = *start_iter;

    // start iter can't be end cause it's prev'd.

    auto frame = Frame{.start =
                           Frame::Timepoint{
                               .context = *this,
                               .handle = start->start_handle,
                           },
                       .end = Frame::Timepoint{
                           .context = *this,
                           .handle = end->end_handle,
                       }};
    this->in_flight_frames.emplace_back(
        std::make_unique<Frame>(std::move(frame)));
}

const auto debug_log_time = [](const auto& diff) {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(diff);
    const auto us = duration_cast<microseconds>(diff - ms);
    const auto ns = duration_cast<nanoseconds>(diff - ms - us);
    std::cerr << ms << " " << us << " " << ns << "\n";
};

void QueueContext::process_frames() {
    if (!std::size(this->in_flight_frames)) {
        return;
    }

    // Collect all devices and call calibrate.
    [this]() -> auto {
        using context_ref_t = std::reference_wrapper<DeviceContext>;
        auto target_devices = std::unordered_map<VkDevice, context_ref_t>{};
        for (const auto& frame : this->in_flight_frames) {
            auto& start = frame->start.context.device_context;
            auto& end = frame->end.context.device_context;

            target_devices.try_emplace(start.device, std::ref(start));
            target_devices.try_emplace(end.device, std::ref(end));
        }
        for (const auto& pair : target_devices) {
            auto& device = pair.second.get();
            device.clock.calibrate();
        }
    }();

    const auto get_tick_time = [](const auto& timepoint)
        -> std::optional<DeviceContext::Clock::time_point_t> {
        const auto& handle = timepoint.handle;
        const auto& context = timepoint.context;

        const auto ticks = handle->get_ticks(*context.timestamp_pool);
        if (!ticks.has_value()) {
            return std::nullopt;
        }
        const auto& clock = context.device_context.clock;
        return clock.ticks_to_time(*ticks);
    };

    std::cerr << "starting frame readout\n";
    while (std::size(this->in_flight_frames)) {
        const auto& frame = this->in_flight_frames.front();
        assert(frame);

        const auto a = get_tick_time(frame->start);
        if (!a.has_value()) {
            break;
        }

        const auto b = get_tick_time(frame->end);
        if (!b.has_value()) {
            break;
        }

        // assert(a <= b);

        //
        const auto last_b =
            this->timings.empty() ? *a : this->timings.back()->gpu_end;

        // assert(last_b <= a);

        const auto frametime = *b - last_b;

        std::cerr
            << "        calculated total time from last frame (frametime): ";
        debug_log_time(*b - last_b);

        this->timings.emplace_back(std::make_unique<Timing>(Timing{
            .gpu_start = *a,
            .gpu_end = *b,
            .frametime = frametime,
        }));

        this->in_flight_frames.pop_front();
    }

    const auto MAX_TRACKED = 50;
    if (std::size(this->timings) < MAX_TRACKED) {
        return;
    }
    this->timings.erase(std::begin(this->timings),
                        std::next(std::begin(this->timings),
                                  std::size(this->timings) - MAX_TRACKED));
}

using opt_time_point_t = std::optional<DeviceContext::Clock::time_point_t>;
opt_time_point_t QueueContext::get_sleep_until() {

    // Call this to push all in flight frames into our timings structure,
    // but only if they're completed. So now they are truly *in flight frames*.
    this->process_frames();
    
    // We have completed all frames. DO NOT WAIT!
    if (!std::size(this->in_flight_frames)) {
        return std::nullopt;
    }

    const auto median_frametime = [&, this]() {
        auto vect = std::vector<Timing*>{};
        std::ranges::transform(this->timings, std::back_inserter(vect),
                               [](const auto& timing) { return timing.get(); });
        std::ranges::sort(vect, [](const auto& a, const auto& b) {
            return a->frametime < b->frametime;
        });
        return vect[std::size(vect) / 2]->frametime;
    }();

    //                                    PRESENT CALL
    // | -------x----- | -------x--------------|
    // ^ last_b        ^ a                     ^ b
    //
    // Us, the CPU on the host, is approximately at 'b'.
    // We have a good guess for the distance between
    // last_b and b (median_frametime).
    // The GPU is at any point on this line (marked as x).
    // Don't use A. It's less robust than just using last_b.
    // It *might* be more accurate because it's closer,
    // but there's an issue where there can sometimes be a very
    // small distance between a and b because it is just the
    // point in time when the vkAcquireSwapchainKHR signals
    // the wait on the gpu queue, which can sometimes be tiny.

    std::cerr << "    median 100 frametimes: ";
    debug_log_time(median_frametime);

    // 2% of average gpu time for dealing with variance.
    // This could be calculated more precisely with the
    // numbers we have (like we could construct a high% confidence
    // interval? not big on maths).
    const auto slack = median_frametime / 50;

    // If we're more than 1 frame queued, then we should wait for
    // that to complete before returning. It's likely way better to
    // to sleep twice here and recompute between sleeps because we're
    // extrapolating really far into the future here! TODO
    const auto extra_delay =
        median_frametime * (std::size(this->in_flight_frames) - 1);

    const auto& last_b = this->timings.back()->gpu_end;

    // All educated guesses:
    //  dist_to_b = frametime - dist_to_last_b;
    //  dist_to_last_b = now - last_b
    //  sleep_until = now + extra_delay + slack + dist_to_b
    //              = now + extra_delay + slack + (frametime - dist_to_last_b)
    //              = now + extra_delay + slack + frametime - (now - last_b)

    const auto now = std::chrono::steady_clock::now();
    assert(last_b <= now);
    const auto dist = now - last_b;
    // Even if this is negative, it's a no-op to sleep backwards.
    return now + extra_delay + slack + median_frametime - dist;
}

} // namespace low_latency