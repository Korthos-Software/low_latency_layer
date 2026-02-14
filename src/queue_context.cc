#include "queue_context.hh"
#include "device_context.hh"
#include "timestamp_pool.hh"

#include <chrono>
#include <iostream>

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
    const VkSubmitInfo& info, const std::uint64_t& target_semaphore_sequence,
    const std::shared_ptr<TimestampPool::Handle> head_handle,
    const std::shared_ptr<TimestampPool::Handle> tail_handle) {

    auto signals = std::unordered_set<VkSemaphore>{};
    auto waits = std::unordered_set<VkSemaphore>{};
    std::ranges::copy_n(info.pWaitSemaphores, info.waitSemaphoreCount,
                        std::inserter(waits, std::end(waits)));
    std::ranges::copy_n(info.pSignalSemaphores, info.signalSemaphoreCount,
                        std::inserter(signals, std::end(signals)));

    this->submissions.emplace_back(std::make_unique<Submission>(
        std::move(signals), std::move(waits), target_semaphore_sequence,
        head_handle, tail_handle));

    // TODO HACK
    if (std::size(this->submissions) > 100) {
        this->submissions.pop_front();
    }
}

/*
void QueueContext::notify_submit(
    std::span<const VkSubmitInfo2> infos,
    const std::uint64_t target_semaphore_sequence,
    std::shared_ptr<TimestampPool::Handle>&& handle) {

    auto signals = std::unordered_set<VkSemaphore>{};
    auto waits = std::unordered_set<VkSemaphore>{};

    for (const auto& info : infos) {
        constexpr auto get_semaphore = [](const auto& semaphore_info) {
            return semaphore_info.semaphore;
        };
        std::ranges::transform(info.pSignalSemaphoreInfos,
                               std::next(info.pSignalSemaphoreInfos,
                                         info.signalSemaphoreInfoCount),
                               std::inserter(signals, std::end(signals)),
                               get_semaphore);
        std::ranges::transform(
            info.pWaitSemaphoreInfos,
            std::next(info.pWaitSemaphoreInfos, info.waitSemaphoreInfoCount),
            std::inserter(waits, std::end(waits)), get_semaphore);
    }

    this->submissions.emplace_back(std::make_unique<Submission>(
        std::move(signals), std::move(waits), target_semaphore_sequence,
        std::move(handle)));

    // TODO HACK
    if (std::size(this->submissions) > 100) {
        this->submissions.pop_front();
    }
}
*/

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

    const auto start_iter = std::ranges::find_if(
        std::rbegin(this->submissions), std::rend(this->submissions),
        [&](const auto& submission) {
            return std::ranges::any_of(
                submission->waits, [&](const auto& wait) {
                    return collected_semaphores.contains(wait);
                });
        });

    if (start_iter == std::rend(this->submissions)) {
        std::cout << "couldn't find starting submission!\n";
        return;
    }
    const auto& start = *start_iter;

    const auto end_iter = std::ranges::find_if(
        std::rbegin(this->submissions), std::rend(this->submissions),
        [&](const auto& submission) {
            return std::ranges::any_of(
                submission->signals,
                [&](const auto& signal) { return waits.contains(signal); });
        });

    if (end_iter == std::rend(this->submissions)) {
        std::cout << "couldn't find ending submission!\n";
        return;
    }
    const auto& end = *end_iter;

    auto frame = Frame{.start =
                           Frame::Timepoint{
                               .context = *this,
                               .handle = start->start_handle,
                               .sequence = start->sequence,
                           },
                       .end = Frame::Timepoint{
                           .context = *this,
                           .handle = end->end_handle,
                           .sequence = end->sequence,
                       }};
    this->in_flight_frames.emplace_back(
        std::make_unique<Frame>(std::move(frame)));

    // hack
    if (this->in_flight_frames.size() > 5) {
        this->in_flight_frames.pop_front();
    }
}

std::optional<QueueContext::duration_t> QueueContext::get_delay_time() {
    if (!std::size(this->in_flight_frames)) {
        return std::nullopt;
    }

    // We are about to query the wait semaphores of all of our current
    // frames in flight. They may come from the same device, so we're going
    // to build a mapping here to reduce vulkan calls. Not only that,
    // we have to do this or else our timing information becomes broken
    // as this loop iterates.
    const auto target_devices = [this]() -> auto {
        using context_ref_t = std::reference_wrapper<DeviceContext>;
        auto target_devices = std::unordered_map<VkDevice, context_ref_t>{};
        for (const auto& frame : this->in_flight_frames) {
            auto& start = frame->start.context.device_context;
            auto& end = frame->end.context.device_context;

            target_devices.try_emplace(start.device, std::ref(start));
            target_devices.try_emplace(end.device, std::ref(end));
        }
        return target_devices;
    }();

    // Calibrate timestamps before we acquire semaphores.
    for (const auto& pair : target_devices) {
        auto& device = pair.second;
        device_context.clock.calibrate();
    }

    // Now we have all owned devices and their clocks are in a good state.
    // We need to build another mapping of semaphores to their queries now.
    const auto queue_sequences = [this]() -> auto {
        auto queue_sequences = std::unordered_map<VkQueue, std::uint64_t>{};
        for (const auto& frame : this->in_flight_frames) {
            auto& start = frame->start.context;
            auto& end = frame->end.context;

            for (const auto& queue_ptr : {&start, &end}) {
                if (queue_sequences.contains(queue_ptr->queue)) {
                    continue;
                }

                const auto& vtable = queue_ptr->device_context.vtable;
                auto seq = std::uint64_t{};
                vtable.GetSemaphoreCounterValueKHR(this->device_context.device,
                                                   this->semaphore, &seq);
                queue_sequences.emplace(queue_ptr->queue, seq);
            }
        }
        return queue_sequences;
    }();

    // Now all devices we are about to query are primed to query.
    // We have all sequence numbers from all queus we could possibly query.
    const auto S = std::size(this->in_flight_frames);
    for (auto i = std::size_t{0}; i < S; ++i) {
        assert(this->in_flight_frames[i]);
        const auto& frame = *this->in_flight_frames[i];
        const auto& start = frame.start;
        const auto& end = frame.end;

        std::cout << "    Evaluating the frame that's " << S - i - 1
                  << " behind\n";

        std::cout << "    target start seq: " << start.sequence << '\n';
        std::cout << "    target end seq: " << end.sequence << '\n';

        const auto start_seq_it = queue_sequences.find(start.context.queue);
        assert(start_seq_it != std::end(queue_sequences));
        const auto& start_seq = start_seq_it->second;
        if (start_seq < start.sequence) {
            std::cout << "        frame hasn't started yet !\n ";
            continue;
        }

        /*
        const auto start_ticks_opt =
            start.handle->get_ticks(*start.context.timestamp_pool);
        if (!start_ticks_opt.has_value()) {
            std::cout << "        frame hasn't started yet !\n ";
        }

        std::cout << "        START TICKS: " << start_ticks << '\n';
        const auto start_time =
            start.context.device_context.clock.ticks_to_time(start_ticks);

        {
            using namespace std::chrono;
            const auto diff = now - a;
            const auto ms = duration_cast<milliseconds>(diff);
            const auto us = duration_cast<microseconds>(diff - ms);
            const auto ns = duration_cast<nanoseconds>(diff - ms - us);
            std::cout << "        frame started: " << ms << " ms " << us
                      << " us " << ns << " ns ago\n";
        }

        const auto end_seq_it = queue_sequences.find(end.context.queue);
        assert(end_seq_it != std::end(queue_sequences));
        const auto& end_seq = end_seq_it->second;
        if (start_seq < end.sequence) {
            std::cout << "        frame hasn't started yet !\n ";
            continue;
        }
        */
    }

    return std::nullopt;
    //
}

// now it's all coming together
// std::optional<QueueContext::duration_t> QueueContext::get_delay_time() {
/*
if (!std::size(this->in_flight_frames)) {
    return std::nullopt;
}

auto seq = std::uint64_t{};
this->device_context.vtable.GetSemaphoreCounterValueKHR(
    this->device_context.device, this->semaphore, &seq);

// Get semaphore first, then poll!
this->timestamp_pool->poll();

// idk how frequently we should call this.
this->device_context.calibrate_timestamps();

static auto gpu_frametimes = std::deque<uint64_t>{};
static auto cpu_frametimes = std::deque<uint64_t>{};

const auto S = std::size(this->in_flight_frames);

std::cout << "\nSTART FRAME READOUT\n";
std::cout << "error bound: " << this->device_context.clock.error_bound
          << '\n';
std::cout << "num frames in flight: " << S << '\n';
std::cout << "from oldest -> newest\n";

// const auto b_seq = semaphore_from_context(*this);
const auto now = std::chrono::steady_clock::now();

auto i = std::size_t{0};
for (; i < std::size(this->in_flight_frames); ++i) {
    const auto& frame = this->in_flight_frames[i];
    std::cout << "    Evaluating the frame that's " << S - i - 1
              << " behind\n";
    if (!frame) {
        std::cout << "        nullptr!\n";
        continue;
    }

    std::cout << "    target start: " << frame->target_start_sequence <<
'\n'; std::cout << "    target end: " << frame->target_end_sequence << '\n'; if
(seq < frame->target_start_sequence) { std::cout << "        frame hasn't
started yet!\n"; continue;
    }

    const auto start_ticks =
        frame->start_context.timestamp_pool->get_polled(*frame->start);
    std::cout << "        START TICKS: " << start_ticks << '\n';
    const auto& a_clock = frame->start_context.device_context.clock;
    const auto a = a_clock.ticks_to_time(start_ticks);

    {
        using namespace std::chrono;
        const auto diff = now - a;
        const auto ms = duration_cast<milliseconds>(diff);
        const auto us = duration_cast<microseconds>(diff - ms);
        const auto ns = duration_cast<nanoseconds>(diff - ms - us);
        std::cout << "        frame started: " << ms << " ms " << us
                  << " us " << ns << " ns ago\n";
    }

    if (seq < frame->target_end_sequence) {
        std::cout << "        frame hasn't ended yet!\n";
        continue;
    }


    const auto end_ticks =
        frame->end_context.timestamp_pool->get_polled(*frame->end, true);
    const auto& b_clock = frame->end_context.device_context.clock;
    std::cout << "        END_TICKS: " << end_ticks << '\n';
    const auto b = b_clock.ticks_to_time(end_ticks);
    {
        using namespace std::chrono;
        if (now <= b) {
            std::cout << "b happened before now?\n";
        }
        const auto diff = now - b;
        const auto ms = duration_cast<milliseconds>(diff);
        const auto us = duration_cast<microseconds>(diff - ms);
        const auto ns = duration_cast<nanoseconds>(diff - ms - us);
        std::cout << "        frame ended: " << ms << " ms " << us
                  << " us " << ns << " ns ago\n";
    }

    const auto gpu_time = b - a;
    {
        using namespace std::chrono;
        const auto diff = gpu_time;
        const auto ms = duration_cast<milliseconds>(diff);
        const auto us = duration_cast<microseconds>(diff - ms);
        const auto ns = duration_cast<nanoseconds>(diff - ms - us);
        std::cout << "        gpu_time: " << ms << " ms " << us
                  << " us " << ns << " ns ago\n";
    }

    /*
    cpu_frametimes.emplace_back(cpu_time);
    gpu_frametimes.emplace_back(gpu_time);
}

/*
if (remove_index.has_value()) {
    this->in_flight_frames.erase(std::begin(this->in_flight_frames),
                                 std::begin(this->in_flight_frames) +
                                     *remove_index);
}
*/

/*
auto g_copy = gpu_frametimes;
auto c_copy = cpu_frametimes;
std::ranges::sort(g_copy);
std::ranges::sort(c_copy);

constexpr auto N = 49;
if (std::size(cpu_frametimes) < N) {
    return std::nullopt;
}

const auto F = std::size(g_copy);
// close enough to median lol
const auto g = g_copy[F / 2];
const auto c = c_copy[F / 2];

std::cout << g << '\n';

std::cout << "    median gpu: " << (g / 1'000'000) << " ms " << g / 1'000
          << " us " << g << " ns\n";
std::cout << "    median cpu: " << c / 1'000'000 << " ms " << c / 1'000
          << " us " << c << " ns\n";

if (F > N) {
    gpu_frametimes.pop_front();
    cpu_frametimes.pop_front();
}

return std::nullopt;
}
*/

} // namespace low_latency