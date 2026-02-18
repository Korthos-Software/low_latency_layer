#include "queue_context.hh"
#include "device_context.hh"
#include "timestamp_pool.hh"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
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
    // with little conseuqence.
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

    this->in_flight_frames.emplace_back(Frame{
        .prev_frame_last_submit = prev_frame_last_submit,
        .submissions = std::move(this->submissions),
        .sequence = (*last_iter)->sequence,
    });
    assert(std::size(this->in_flight_frames.back().submissions));
    // *valid but unspecified state after move, so clear!*
    this->submissions.clear();
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

    // We used to collect all devices that were pointed to by all potential
    // submissions, put them in a set and then call.calibrate() on each once.
    // This is unnecessary now - we can assume all submissions come from the
    // same queue (this one!).
    auto& device_context = this->device_context;
    auto& clock = device_context.clock;
    clock.calibrate();

    // Get the queue's sequence number so we can quickly check
    // frames are finished without calling getCalibratedTimestamps.
    // This is somewhat a premature optimization but it's elegant.
    const auto seq = [&, this]() -> auto {
        auto seq = std::uint64_t{0};
        device_context.vtable.GetSemaphoreCounterValueKHR(
            device_context.device, this->semaphore, &seq);
        return seq;
    }();

    while (std::size(this->in_flight_frames)) {
        const auto& frame = this->in_flight_frames.front();

        // There should be at least one submission, we guarantee it in
        // notify_present.
        assert(std::size(frame.submissions));

        const auto& last_submission = frame.submissions.back();

        // Not completed (so future frames definitely aren't) - stop early.
        if (seq < last_submission->sequence) {
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
                    const auto get_time = [&, this](const auto& handle) {
                        return handle->get_time();
                    };

                    return Interval{
                        .start = get_time(submission->start_handle),
                        .end = get_time(submission->end_handle),
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

        const auto gputime = std::ranges::fold_left(
            merged, DeviceContext::Clock::time_point_t::duration{},
            [](auto gputime, const auto& interval) {
                const auto& [start, end] = interval;
                return gputime + (end - start);
            });

        // The start should be the previous frame's last submission, NOT when we
        // start here. Otherwise, we won't account for the time between the
        // previous frame's last submission and our first submission, which
        // could genuinely be a large period of time. So look for it in timings,
        // because it's guaranteed to be there at this stage. Either that, or
        // we're the first frame, and it doesn't really matter.
        const auto start = frame.prev_frame_last_submit->end_handle->get_time();
        const auto end = merged.back().end;
        const auto not_gputime = (end - start) - gputime;

        auto timing = Timing{
            .gputime = gputime,
            .not_gputime = not_gputime,
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

    if (const auto F = std::size(this->in_flight_frames); F > 1) {
        // In this case, we are so far ahead that there are multiple frames
        // in flight. Either that, or our bookkeeping has gone horribly
        // wrong! Wait on the 2nd last frame in flight to complete. This
        // shunts us to F=1.
        const auto second_iter = std::next(std::rbegin(this->in_flight_frames));
        assert(second_iter != std::rend(this->in_flight_frames));

        const auto swi = VkSemaphoreWaitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &this->semaphore,
            .pValues = &second_iter->sequence,
        };
        vtable.WaitSemaphoresKHR(device.device, &swi,
                                 std::numeric_limits<std::uint64_t>::max());

        // Here
        this->process_frames(); // get rid of completed frames
    } else if (!F) {
        // We have completed all frames. DO NOT WAIT!
        return;
    }

    // We are checking size again because process_frames might have drained
    // it to zero.
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
    const auto expected_not_gputime =
        calc_median([](const auto& timing) { return timing->not_gputime; });

    std::cerr << "    expected gputime: ";
    debug_log_time(expected_gputime);
    std::cerr << "    expected not_gputime: ";
    debug_log_time(expected_not_gputime);

    //                               PRESENT CALL
    // |----------------------------------|----------------|
    // first                              b                c
    //
    // Us, the CPU on the host, is approximately at 'b'. We have a good
    // guess for the distance between a and b as gputime.

    const auto& frame = this->in_flight_frames.back();

    // We could be in the period where A hasn't signalled yet.
    // It's impossible to make a decision until we know a.
    // Doing this is fine because it won't affect throughput at all.
    // (ie, there's more work queued after regardless).
    // FIXME: If a == b, then we're waiting for the entire queue
    // to finish because the semaphore only says if it has finished.
    // The fix is to check the start timestamp instead of the query
    // in the case that it's...
    // Honestly it might be better to signal two semaphores because
    // we need to wait for when the submission starts work and
    // right now, we only signal when the submission finishes work.
    // Ideally we have both, so we can elegantly wait on the start
    // semaphore of A, then get A's start timestamp. This is BROKEN.

    [&]() -> void {
        const auto swi = VkSemaphoreWaitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &this->semaphore,
            .pValues = &frame.submissions.front()->sequence,
        };
        vtable.WaitSemaphoresKHR(device.device, &swi,
                                 std::numeric_limits<std::uint64_t>::max());
    }();

    // We now know that A is available because its semaphore has been
    // signalled.
    const auto a = frame.prev_frame_last_submit->end_handle->get_time();

    const auto now = std::chrono::steady_clock::now();
    const auto dist = now - a;
    const auto expected = expected_gputime - dist;

    const auto swi = VkSemaphoreWaitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &this->semaphore,
        .pValues = &frame.sequence,
    };
    vtable.WaitSemaphoresKHR(device.device, &swi,
                             std::max(expected.count(), 0l));
}

} // namespace low_latency