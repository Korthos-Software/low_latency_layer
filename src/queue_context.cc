#include "queue_context.hh"
#include "device_context.hh"
#include "timestamp_pool.hh"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <ranges>
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

QueueContext::QueueContext(DeviceContext& device_context, const VkQueue& queue,
                           const std::uint32_t& queue_family_index)
    : device_context(device_context), queue(queue),
      queue_family_index(queue_family_index),
      // Important we make the command pool before the timestamp pool, because
      // it's a dependency.
      command_pool(make_command_pool(device_context, queue_family_index)),
      timestamp_pool(std::make_unique<TimestampPool>(*this)) {}

QueueContext::~QueueContext() {

    this->in_flight_frames.clear();
    this->submissions.clear();
    this->timestamp_pool.reset();

    const auto& vtable = this->device_context.vtable;
    vtable.DestroyCommandPool(this->device_context.device, this->command_pool,
                              nullptr);
}

void QueueContext::notify_submit(
    const VkSubmitInfo& info,
    const std::shared_ptr<TimestampPool::Handle> head_handle,
    const std::shared_ptr<TimestampPool::Handle> tail_handle,
    const DeviceContext::Clock::time_point_t& now) {

    auto signals = std::unordered_set<VkSemaphore>{};
    auto waits = std::unordered_set<VkSemaphore>{};
    std::ranges::copy(std::span{info.pWaitSemaphores, info.waitSemaphoreCount},
                      std::inserter(waits, std::end(waits)));
    std::ranges::copy(
        std::span{info.pSignalSemaphores, info.signalSemaphoreCount},
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

    this->submissions.emplace_back(std::make_unique<Submission>(
        std::move(signals), std::move(waits), head_handle, tail_handle, now));

    // TODO HACK
    if (std::size(this->submissions) > 100) {
        this->submissions.pop_front();
    }
}

void QueueContext::notify_submit(
    const VkSubmitInfo2& info,
    const std::shared_ptr<TimestampPool::Handle> head_handle,
    const std::shared_ptr<TimestampPool::Handle> tail_handle,
    const DeviceContext::Clock::time_point_t& now) {

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
        std::move(signals), std::move(waits), head_handle, tail_handle, now));

    // TODO HACK
    if (std::size(this->submissions) > 100) {
        this->submissions.pop_front();
    }
}

void QueueContext::notify_present(const VkPresentInfoKHR& info) {

    // We are going to assume that all queue submissions before and on the same
    // queue contribute to the frame.

    // This used to be more complicated where we found the first submission that
    // was signalled by acquire, then we walked forwards until we found the
    // submission before it that marked the end of frame (which was the last
    // submission in the previous frame that called notify submit). This seemed
    // completely redundant, in all cases it was exactly what we have here. But
    // I could be wrong.

    const auto start_iter = std::begin(this->submissions);
    // no op submit?
    if (start_iter == std::end(this->submissions)) {
        std::cerr << "ignored no op submit\n";
        return;
    }
    const auto last_iter = std::prev(std::end(this->submissions));

    (*start_iter)->debug += "first_during_present ";
    (*last_iter)->debug += "last_during_present ";

    // The last submission is either in flight, already processed, or we
    // just happen to be the first frame and we can just set it to our start
    // with little consequence.
    const auto prev_frame_last_submit = [&]() -> auto {
        if (const auto iter = std::rbegin(this->in_flight_frames);
            iter != std::rend(this->in_flight_frames)) {

            assert(!iter->submissions.empty());
            return iter->submissions.back();
        }

        if (const auto iter = std::rbegin(this->timings);
            iter != std::rend(this->timings)) {

            const auto& submissions = (*iter)->frame.submissions;
            assert(!submissions.empty());

            return submissions.back();
        }

        return *start_iter;
    }();

    this->in_flight_frames.emplace_back(
        Frame{.submissions = std::move(this->submissions),
              .cpu_post_present_time = std::chrono::steady_clock::now()});
    assert(std::size(this->in_flight_frames.back().submissions));
    // *valid but unspecified state after move, so clear!*
    this->submissions.clear();
}

const auto debug_log_time2 = [](auto& stream, const auto& diff) {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(diff);
    const auto us = duration_cast<microseconds>(diff - ms);
    const auto ns = duration_cast<nanoseconds>(diff - ms - us);
    stream << ms << " " << us << " " << ns << " ago\n";
};

const auto debug_log_time = [](const auto& diff) {
    debug_log_time2(std::cerr, diff);
};

void QueueContext::process_frames() {
    if (!std::size(this->in_flight_frames)) {
        return;
    }

    // We used to collect all devices that were pointed to by all potential
    // submissions, put them in a set and then call.calibrate() on each once.
    // This is unnecessary now - we assume all submissions come from the same
    // queue. FIXME: don't assume this.
    auto& device_context = this->device_context;
    auto& clock = device_context.clock;
    clock.calibrate();

    while (std::size(this->in_flight_frames)) {
        const auto& frame = this->in_flight_frames.front();

        // There should be at least one submission, we guarantee it in
        // notify_present.
        assert(std::size(frame.submissions));

        const auto& last_submission = frame.submissions.back();

        // Not completed (so future frames definitely aren't) - stop early.
        if (!last_submission->end_handle->get_time().has_value()) {
            break;
        }

        // We are committed to removing the frame at this stage and
        // promoting it to a 'timing' struct because it's completed.
        // We can guarantee that we can extract timing information from
        // all start/end handles now.

        // Using leetcode merge intervals in the wild lol
        struct Interval {
            DeviceContext::Clock::time_point_t start, end;
        };

        const auto sorted_intervals = [&, this]() -> auto {
            auto intervals = std::vector<Interval>{};
            std::ranges::transform(
                frame.submissions, std::back_inserter(intervals),
                [&, this](const auto& submission) {
                    return Interval{
                        .start = submission->start_handle->get_time_required(),
                        .end = submission->end_handle->get_time_required(),
                    };
                });

            std::ranges::sort(intervals, [](const auto& a, const auto& b) {
                return a.start < b.start;
            });
            return intervals;
        }();

        const auto merged = [&]() -> auto {
            auto merged = std::vector<Interval>{};
            auto last = sorted_intervals[0];

            for (const auto& [s, e] : sorted_intervals | std::views::drop(1)) {
                if (s <= last.end) {
                    last.end = std::max(last.end, e);
                } else {
                    merged.push_back(last);
                    last = {s, e};
                }
            }
            merged.push_back(last);
            return merged;
        }();

        // It's important to note that gputime starts from a point which isn't
        // equal to the below 'start' var. It looks something like this, where a
        // '-' represents CPU time only and '=' represents CPU + GPU.
        //
        //   |---------------------|=========|--------|====|-----------------|
        //   ^ last_present        ^ merged.front().start            present ^
        //                               merged.back().end ^
        //
        // I would imagine there would be more GPU than cpu to reach the anti
        // lag codepath than is depicted here. We can track the total time
        // between vkPresent calls as future_submit - last_submit. The total
        // time the GPU spent engaged is the sum of all intervals. So we can
        // get a meaningful 'not_gputime' as total - gpu_time.

        const auto gputime = std::ranges::fold_left(
            merged, DeviceContext::Clock::time_point_t::duration{},
            [](auto gputime, const auto& interval) {
                const auto& [start, end] = interval;
                return gputime + (end - start);
            });

        // Our cpu_start value here refers to the time when the CPU was allowed
        // to move past the present call and, in theory, begin cpu work on the
        // next frame.
        const auto cpu_start = [&]() -> auto {
            if (const auto it = std::rbegin(this->timings);
                it != std::rend(this->timings)) {
                return (*it)->frame.cpu_post_present_time;
            }
            // This will happen *once*, and only for the first frame. We don't
            // have a way of knowing when the CPU first started work obviously
            // in this case because we're a vulkan layer and not omniscient.
            // Just return our first submit's start for this edge case.
            return frame.submissions.front()->start_handle->get_time_required();
        }();

        const auto cputime =
            frame.submissions.front()->enqueued_time - cpu_start;

        auto timing = Timing{
            .gputime = gputime,
            .cputime = cputime,
            .frame = frame,
        };
        this->timings.emplace_back(std::make_unique<Timing>(timing));

        this->in_flight_frames.pop_front();
    }

    if (const auto T = std::size(this->timings);
        T > this->MAX_TRACKED_TIMINGS) {

        const auto dist = T - this->MAX_TRACKED_TIMINGS;
        const auto erase_to_iter = std::next(std::begin(this->timings), dist);
        this->timings.erase(std::begin(this->timings), erase_to_iter);
    }
}

void QueueContext::sleep_in_present() {
    const auto& device = this->device_context;
    const auto& vtable = device.vtable;

    // Call this to push all in flight frames into our timings structure,
    // but only if they're completed. So now they are truly *in flight
    // frames*.
    this->process_frames();

    if (!std::size(this->in_flight_frames)) {
        return;
    }

    // This is getting the most recent frame and waiting until its start has
    // begun. This means that, in the case of >1 frame in flight, it's draining
    // all of them before we're allowed to move forward.
    const auto first_gpu_work = [&]() -> auto {
        const auto& most_recent_frame = this->in_flight_frames.back();
        const auto& first_submission = most_recent_frame.submissions.front();
        return first_submission->start_handle->get_time_spinlock();
    }();

    // Process frames because as stated above, we might have multiple frames
    // now completed.
    this->process_frames();

    // Check the size again because the frame we want to target may have already
    // completed when we called process_frames().
    if (!std::size(this->in_flight_frames)) {
        return;
    }
    assert(std::size(this->in_flight_frames) == 1);

    // Not enough data yet to apply any delays.
    if (std::size(this->timings) < this->MAX_TRACKED_TIMINGS) {
        return;
    }

    const auto calc_median = [&, this](const auto& getter) {
        auto vect = std::vector<Timing*>{};
        std::ranges::transform(this->timings, std::back_inserter(vect),
                               [](const auto& timing) { return timing.get(); });
        std::ranges::sort(vect, [&](const auto& a, const auto& b) {
            return getter(a) < getter(b);
        });
        return getter(vect[std::size(vect) / 2]);
    };

    const auto expected_gputime =
        calc_median([](const auto& timing) { return timing->gputime; });
    const auto expected_cputime =
        calc_median([](const auto& timing) { return timing->cputime; });

    // Should look like this:
    //              total_length = expected_gputime
    // |------------------------x------------------------------|
    // ^ first_gpu_work        now               last_gpu_work ^

    const auto now = std::chrono::steady_clock::now();
    const auto dist = now - first_gpu_work;
    const auto expected_dist_to_last = expected_gputime - dist;

    const auto wait_time = expected_dist_to_last - expected_cputime;

    auto& frame = this->in_flight_frames.back();
    const auto& last_gpu_work = frame.submissions.back()->end_handle;
    last_gpu_work->get_time_spinlock(now + wait_time);

    frame.cpu_post_present_time = std::chrono::steady_clock::now();

    std::ofstream f("/tmp/times.txt", std::ios::trunc);
    f << "    expected gputime: ";
    debug_log_time2(f, expected_gputime);
    f << "    expected cputime: ";
    debug_log_time2(f, expected_cputime);
    f << "    requestd sleep: ";
    debug_log_time2(f, wait_time);
    f << "    observed sleep: ";
    debug_log_time2(f, frame.cpu_post_present_time - now);
}

bool QueueContext::should_inject_timestamps() const {
    const auto& pd = this->device_context.physical_device;

    assert(pd.queue_properties);
    const auto& queue_props = *pd.queue_properties;
    assert(this->queue_family_index < std::size(queue_props));

    const auto& props = queue_props[this->queue_family_index];
    // Probably need at least 64, don't worry about it just yet and just ensure
    // it's not zero (because that will cause a crash if we inject).
    return props.queueFamilyProperties.timestampValidBits;
}

} // namespace low_latency