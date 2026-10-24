#include "framegen_pacer.hh"

#include <algorithm>
#include <thread>
#include <vector>

namespace low_latency {

namespace {

// The finished simulation may wait this long for the render thread.
constexpr auto SIMULATION_SLACK_TARGET = std::chrono::microseconds{500};
// Time the render thread may spend blocked in the frame generation's Present.
constexpr auto PRESENT_TARGET = std::chrono::microseconds{2000};
// Kept free so the simulation never becomes the bottleneck.
constexpr auto PERIOD_MARGIN = std::chrono::microseconds{300};
// Above this, the render thread waited for the simulation: back off.
constexpr auto RENDER_WAIT_LIMIT = std::chrono::microseconds{300};
constexpr auto GAIN = 0.25;
constexpr auto TRACKED_FRAMES = std::size_t{16};

void ewma(DeviceClock::duration& avg, const DeviceClock::duration& sample) {
    avg = avg.count() ? DeviceClock::duration{static_cast<DeviceClock::rep>(
                            0.9 * static_cast<double>(avg.count()) +
                            0.1 * static_cast<double>(sample.count()))}
                      : sample;
}

template <typename M> void trim(M& map) {
    while (std::size(map) > TRACKED_FRAMES) {
        map.erase(std::begin(map));
    }
}

} // namespace

FramegenPacer::FramegenPacer() {}

FramegenPacer::~FramegenPacer() {}

void FramegenPacer::notify_marker(const std::uint64_t frame,
                                  const VkLatencyMarkerNV marker) {

    const auto now = DeviceClock::now();
    const auto lock = std::scoped_lock{this->mutex};
    auto& m = this->frame_marks[frame];
    const auto set = [&](auto& t) {
        if (!t.has_value()) {
            t = now;
        }
    };
    switch (marker) {
    case VK_LATENCY_MARKER_SIMULATION_START_NV:
        set(m.simulation_start);
        break;
    case VK_LATENCY_MARKER_SIMULATION_END_NV:
        set(m.simulation_end);
        break;
    case VK_LATENCY_MARKER_RENDERSUBMIT_START_NV:
        set(m.rendersubmit_start);
        break;
    case VK_LATENCY_MARKER_PRESENT_START_NV:
        set(m.present_start);
        break;
    case VK_LATENCY_MARKER_PRESENT_END_NV:
        set(m.present_end);
        break;
    default:
        break;
    }
    trim(this->frame_marks);
}

void FramegenPacer::notify_out_of_band_submit(
    const std::uint64_t frame,
    const std::shared_ptr<TimestampPool::Handle>& handle) {

    const auto lock = std::scoped_lock{this->mutex};
    // Submissions arrive in order: the last one wins.
    this->out_of_band_last[frame] = handle;
    trim(this->out_of_band_last);
}

void FramegenPacer::hold(const DeviceClock::time_point& sleep_called) {
    using namespace std::chrono;

    // a: finished simulation waiting for the render thread, b: render thread
    // waiting for the simulation, c: render thread blocked in Present.
    auto a = std::optional<DeviceClock::duration>{};
    auto b = std::optional<DeviceClock::duration>{};
    auto c = std::optional<DeviceClock::duration>{};
    auto pending = std::vector<
        std::pair<std::uint64_t, std::shared_ptr<TimestampPool::Handle>>>{};
    {
        const auto lock = std::scoped_lock{this->mutex};

        // The newest frame with SIMULATION_END and RENDERSUBMIT_START whose
        // predecessor has PRESENT_END.
        for (auto it = std::rbegin(this->frame_marks);
             it != std::rend(this->frame_marks); ++it) {

            const auto& [frame, m] = *it;
            if (frame <= this->last_used_frame || !m.simulation_end ||
                !m.rendersubmit_start) {
                continue;
            }
            const auto prev = this->frame_marks.find(frame - 1);
            if (prev == std::end(this->frame_marks) ||
                !prev->second.present_end) {
                continue;
            }
            const auto& p = prev->second;
            a = *m.rendersubmit_start - *m.simulation_end;
            b = *m.rendersubmit_start - *p.present_end;
            if (p.present_start && *p.present_end > *p.present_start) {
                c = *p.present_end - *p.present_start;
            }
            if (p.simulation_start && m.simulation_start) {
                const auto period = *m.simulation_start - *p.simulation_start;
                if (period > 0ns && period < 100ms) {
                    ewma(this->simulation_period, period);
                }
            }
            if (m.simulation_start && *m.simulation_end > *m.simulation_start) {
                ewma(this->simulation_time,
                     *m.simulation_end - *m.simulation_start);
            }
            this->last_used_frame = frame;
            break;
        }

        for (const auto& [frame, handle] : this->out_of_band_last) {
            if (!this->out_of_band_end.contains(frame)) {
                pending.emplace_back(frame, handle);
            }
        }
    }

    // GPU frame period from the completion of consecutive frames' out-of-band
    // work (non-blocking).
    for (const auto& [frame, handle] : pending) {
        const auto end = handle->try_end();
        if (!end) {
            continue;
        }
        const auto lock = std::scoped_lock{this->mutex};
        this->out_of_band_end[frame] = *end;
        const auto prev = this->out_of_band_end.find(frame - 1);
        if (prev != std::end(this->out_of_band_end)) {
            const auto period = *end - prev->second;
            if (period > 1ms && period < 100ms) {
                ewma(this->gpu_period, period);
            }
        }
        trim(this->out_of_band_end);
    }

    const auto to_ns = [](const DeviceClock::duration& d) {
        return static_cast<double>(duration_cast<nanoseconds>(d).count());
    };
    const auto locked =
        this->gpu_period.count() && this->simulation_time.count();
    if (a.has_value()) {
        if (locked && c.has_value()) {
            // Period-locked simulation start, trimmed by the Present slack.
            const auto cap = std::max(
                0.0, to_ns(this->gpu_period - this->simulation_time -
                           PERIOD_MARGIN));
            this->delay_ns = std::clamp(
                cap + 0.5 * to_ns(*c - PRESENT_TARGET), 0.0, cap);
        } else if (!locked) {
            if (*b > RENDER_WAIT_LIMIT) {
                this->delay_ns -= 0.5 * to_ns(*b);
            } else {
                this->delay_ns += GAIN * to_ns(*a - SIMULATION_SLACK_TARGET);
            }
            this->delay_ns = std::clamp(
                this->delay_ns, 0.0, 0.9 * to_ns(this->simulation_period));
        }
    }

    const auto release =
        std::max(DeviceClock::now(),
                 sleep_called + nanoseconds{static_cast<std::int64_t>(
                                    this->delay_ns)});
    while (DeviceClock::now() < release) {
        const auto left = release - DeviceClock::now();
        if (left > 300us) {
            std::this_thread::sleep_for(left - 200us);
        } else {
            std::this_thread::yield();
        }
    }
}

} // namespace low_latency
