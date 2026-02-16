#include "queue_context.hh"
#include "device_context.hh"
#include "timestamp_pool.hh"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
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
    const VkSubmitInfo& info, const std::uint64_t& sequence,
    const std::shared_ptr<TimestampPool::Handle> head_handle,
    const std::shared_ptr<TimestampPool::Handle> tail_handle) {

    auto signals = std::unordered_set<VkSemaphore>{};
    auto waits = std::unordered_set<VkSemaphore>{};
    std::ranges::copy_n(info.pWaitSemaphores, info.waitSemaphoreCount,
                        std::inserter(waits, std::end(waits)));
    std::ranges::copy_n(info.pSignalSemaphores, info.signalSemaphoreCount,
                        std::inserter(signals, std::end(signals)));

    std::cerr << "submit1 notif for queue " << this->queue << '\n';
    std::cerr << "    signals: \n";
    for (const auto& signal : signals) {
        std::cerr << "      " << signal << '\n';
    }
    std::cerr << "    waits: \n";
    for (const auto& wait : waits) {
        std::cerr << "      " << wait << '\n';
    }

    this->submissions.emplace_back(
        std::make_unique<Submission>(std::move(signals), std::move(waits),
                                     head_handle, tail_handle, sequence));

    // TODO HACK
    if (std::size(this->submissions) > 100) {
        this->submissions.pop_front();
    }
}

void QueueContext::notify_submit(
    const VkSubmitInfo2& info, const std::uint64_t& sequence,
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

    this->submissions.emplace_back(
        std::make_unique<Submission>(std::move(signals), std::move(waits),
                                     head_handle, tail_handle, sequence));

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
    (*acquire_iter)->debug += "acquire ";

    const auto present_iter = std::ranges::find_if(
        std::rbegin(this->submissions), std::rend(this->submissions),
        [&](const auto& submission) {
            return std::ranges::any_of(
                submission->signals, [&](const auto& signal) {
                    if (waits.contains(signal)) {
                        std::cerr
                            << "queue with signal matching present iter found: "
                            << signal << '\n';
                    }

                    return waits.contains(signal);
                });
        });

    if (present_iter == std::rend(this->submissions)) {
        std::cerr << "couldn't find ending submission!\n";
        return;
    }
    (*present_iter)->debug += "present ";
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
    const auto after_previous_present_iter = std::prev(std::ranges::find_if(
        std::next(acquire_iter), std::rend(this->submissions),
        [](const auto& submission) {
            return submission->end_of_frame_marker;
        }));

    (*after_previous_present_iter)->debug += "after_previous_present ";

    this->submissions.back()->debug += "last_in_host_present ";

    std::cerr << "the present iter was dist from back of: "
              << std::distance(std::rbegin(this->submissions), present_iter)
              << '\n';
    std::cerr << "the acquire iter was dist from back of: "
              << std::distance(std::rbegin(this->submissions), acquire_iter)
              << '\n';
    std::cerr << "the before_previous_present iter was dist from back of: "
              << std::distance(std::rbegin(this->submissions),
                               after_previous_present_iter)
              << '\n';

    auto frame =
        Frame{.start =
                  Frame::Timepoint{
                      .context = *this,
                      .handle = (*after_previous_present_iter)->start_handle,
                      .sequence = (*after_previous_present_iter)->sequence,
                  },
              .end = Frame::Timepoint{
                  .context = *this,
                  .handle = (*present_iter)->end_handle,
                  .sequence = (*present_iter)->sequence,
              }};
    this->in_flight_frames.emplace_back(
        std::make_unique<Frame>(std::move(frame)));
}

const auto debug_log_time = [](const auto& diff) {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(diff);
    const auto us = duration_cast<microseconds>(diff - ms);
    const auto ns = duration_cast<nanoseconds>(diff - ms - us);
    std::cerr << ms << " " << us << " " << ns << " ago\n";
};

void QueueContext::process_frames() {
    if (!std::size(this->in_flight_frames)) {
        return;
    }

    // Collect all devices and call calibrate.
    [this]() -> void {
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

        // We are committed to removing the frame at this stage and promoting it
        // to a 'timing' struct because it's completed..

        const auto frametime = *b - *a;

        const auto cpu_time =
            [&]() -> DeviceContext::Clock::time_point_t::duration {
            const auto latest_iter = std::rbegin(this->timings);
            if (latest_iter == std::rend(this->timings)) {
                return DeviceContext::Clock::time_point_t::duration::zero();
            }
            return *a - (*latest_iter)->gpu_end;
        }();

        std::cerr
            << "        calculated total time from last frame (frametime): ";
        debug_log_time(*b - *a);

        this->timings.emplace_back(std::make_unique<Timing>(
            Timing{.gpu_end = *b,
                   .gpu_time = frametime,
                   .cpu_time = cpu_time,
                   .frame = std::move(this->in_flight_frames.front())}));
        this->in_flight_frames.pop_front();
    }

    if (const auto T = std::size(this->timings);
        T > this->MAX_TRACKED_TIMINGS) {

        const auto dist = T - this->MAX_TRACKED_TIMINGS;
        const auto erase_to_iter = std::next(std::begin(this->timings), dist);
        this->timings.erase(std::begin(this->timings), erase_to_iter);
    }
}

using opt_time_point_t = std::optional<DeviceContext::Clock::time_point_t>;
void QueueContext::sleep_in_present() {
    const auto& device = this->device_context;
    const auto& vtable = device.vtable;

    // Call this to push all in flight frames into our timings structure,
    // but only if they're completed. So now they are truly *in flight frames*.
    this->process_frames();

    if (const auto F = std::size(this->in_flight_frames); F > 1) {
        // In this case, we are so far ahead that there are multiple frames in
        // flight. Either that, or our bookkeeping has gone horribly wrong! Wait
        // on the 2nd last frame in flight to complete. This shunts us to F=1.
        const auto second_iter = std::next(std::rbegin(this->in_flight_frames));
        assert(second_iter != std::rend(this->in_flight_frames));

        const auto& frame = (*second_iter)->end.sequence;
        const auto swi = VkSemaphoreWaitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &this->semaphore,
            .pValues = &(*second_iter)->end.sequence,
        };
        vtable.WaitSemaphoresKHR(device.device, &swi,
                                 std::numeric_limits<std::uint64_t>::max());

        // Here
        this->process_frames(); // get rid of completed frames
    } else if (!F) {
        // We have completed all frames. DO NOT WAIT!
        return;
    }

    if (!std::size(this->in_flight_frames)) {
        return;
    }
    assert(std::size(this->in_flight_frames) == 1);

    // Not enough data yet to apply any delays.
    if (std::size(this->timings) < this->MAX_TRACKED_TIMINGS) {
        return;
    }

    const auto expected_gputime = [&, this]() {
        auto vect = std::vector<Timing*>{};
        std::ranges::transform(this->timings, std::back_inserter(vect),
                               [](const auto& timing) { return timing.get(); });
        std::ranges::sort(vect, [](const auto& a, const auto& b) {
            return a->gpu_time < b->gpu_time;
        });
        // return vect[0]->frametime;
        return vect[std::size(vect) / 2]->gpu_time;
    }();

    const auto expected_cputime = [&, this]() {
        auto vect = std::vector<Timing*>{};
        std::ranges::transform(this->timings, std::back_inserter(vect),
                               [](const auto& timing) { return timing.get(); });
        std::ranges::sort(vect, [](const auto& a, const auto& b) {
            return a->cpu_time < b->cpu_time;
        });
        // return vect[0]->frametime;
        return vect[std::size(vect) / 2]->cpu_time;
    }();
    std::cerr << "    expected gputime: ";
    debug_log_time(expected_gputime);
    std::cerr << "    expected cputime: ";
    debug_log_time(expected_cputime);

    //                               PRESENT CALL
    // |--------------|-------------------|----------------|
    // a        swap_acquire              b                c
    //
    // Us, the CPU on the host, is approximately at 'b'. We have a good guess
    // for the distance between a and b as gputime. Our educated guess for the
    // distance between b and c is cputime. The GPU is between a and b.

    const auto& frame = this->in_flight_frames.back();

    // We could be in the period where A hasn't signalled yet.
    // It's impossible to make a decision until we know a.
    // Doing this is fine because it won't affect throughput at all.
    // (ie, there's more work queued after regardless).
    [&]() -> void {
        const auto swi = VkSemaphoreWaitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &this->semaphore,
            .pValues = &frame->start.sequence,
        };
        vtable.WaitSemaphoresKHR(device.device, &swi,
                                 std::numeric_limits<std::uint64_t>::max());
    }();

    // We now know that A is available because its semaphore has been signalled.
    const auto a_handle = frame->start.handle;
    const auto& a_context = frame->start.context;

    const auto a_ticks_opt = a_handle->get_ticks(*a_context.timestamp_pool);
    assert(a_ticks_opt.has_value());
    const auto& a_clock = a_context.device_context.clock;
    const auto a = a_clock.ticks_to_time(*a_ticks_opt);

    const auto now = std::chrono::steady_clock::now();
    const auto dist = now - a;
    const auto expected = expected_gputime - dist - expected_cputime;

    const auto swi = VkSemaphoreWaitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &this->semaphore,
        .pValues = &frame->end.sequence,
    };
    vtable.WaitSemaphoresKHR(device.device, &swi,
                             std::max(expected.count(), 0l));

    /*

        // 2% of average gpu time for dealing with variance.
        // This could be calculated more precisely with the
        // numbers we have (like we could construct a high% confidence
        // interval? not big on maths).
        const auto slack = gputime / 50;

        // All educated guesses:
        //  dist_to_b = gputime - dist_to_last_b;
        //  dist_to_last_b = now - last_b
        //  sleep_until = now + extra_delay + slack + dist_to_b
        //              = now + extra_delay + slack + (frametime -
       dist_to_last_b)
        //              = now + extra_delay + slack + frametime - (now - last_b)

        const auto now = std::chrono::steady_clock::now();
        assert(last_b <= now);
        const auto dist = now - last_b;
        // Even if this is negative, it's a no-op to sleep backwards.
        const auto sleep_target =
            now + extra_delay + slack + median_frametime - dist;
            */

    /*
    std::cerr << " SLEEPING FOR: ";
    debug_log_time(sleep_target - now);
    std::this_thread::sleep_until(sleep_target);
    */

    /*
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

    const auto finished_time =
        get_tick_time(this->in_flight_frames.back()->end);
    const auto after = std::chrono::steady_clock::now();
    if (!finished_time.has_value()) {
        std::cerr << "didnt finish late!\n";
    } else {
        std::cerr << "finished late by: ";
        const auto late_time = after - *finished_time;
    }

    const auto get_tick_time2 = [this](const auto& handle)
        -> std::optional<DeviceContext::Clock::time_point_t> {
        const auto& context = *this;

        const auto ticks = handle->get_ticks(*context.timestamp_pool);
        if (!ticks.has_value()) {
            return std::nullopt;
        }
        const auto& clock = context.device_context.clock;
        return clock.ticks_to_time(*ticks);
    };

    this->submissions.back()->debug += " last_in_wait";

    std::cerr << "---------------SUBMISSION READOUT POST FRAME SUBMIT AND "
                 "WAIT--------------------\n";
    auto i = std::size_t{0};
    for (auto it = std::rbegin(this->submissions);
         i < 20 && it != std::rend(this->submissions); ++it, ++i) {

        std::cerr << "    submission -" << i << '\n';
        const auto& submission = **it;

        std::cerr << "        sequence target: " << submission.sequence << '\n';

        std::cerr << "        debug tags: " << submission.debug << '\n';

        const auto end_time = get_tick_time2(submission.end_handle);
        std::cerr << "        end_time: ";
        if (end_time.has_value()) {
            debug_log_time(after - *end_time);
        } else {
            std::cerr << "not finished yet!\n";
        }

        const auto start_time = get_tick_time2(submission.start_handle);
        std::cerr << "        start_time: ";
        if (start_time.has_value()) {
            debug_log_time(after - *start_time);
        } else {
            std::cerr << "not finished yet!\n";
        }
    }
    */
}

} // namespace low_latency