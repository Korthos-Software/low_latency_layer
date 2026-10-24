#ifndef FRAMEGEN_PACER_HH_
#define FRAMEGEN_PACER_HH_

#include "device_clock.hh"
#include "timestamp_pool.hh"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace low_latency {

// With frame generation (DLSS-G, or anything else that submits on an
// out-of-band queue), waiting for the GPU work of the last present does not
// hold the simulation back: the generated frames are presented after the real
// frame's work, so by the time the application sleeps, that work has always
// finished and the sleep returns immediately. The game then runs about three
// frames deep. Measured in The Witcher 3 with DLSS-G 4x on an RX 9070: the
// finished simulation waits ~12ms for the render thread, which then waits
// ~10ms inside the frame generation's Present.
//
// This pacer adds a hold after the normal sleep (opt-in,
// LOW_LATENCY_LAYER_FRAMEGEN_PACING=1):
//
//  - With out-of-band submissions, the simulation start is locked to the GPU
//    frame period T (from the completion times of consecutive frames'
//    out-of-band work): delay = T - s - margin + (c - target) / 2, capped at
//    T - s - margin, where s is the simulation time and c the time the render
//    thread spent blocked in Present. The cap means the simulation loop can
//    never run slower than the GPU. Same scene, simulation start to first
//    refresh showing the frame: 4x 41.4ms -> 26.1ms, 2x 32.3ms -> 22.1ms, no
//    throughput change.
//
//  - Without them (frame generation off, or one that does not use an
//    out-of-band queue) it trims the time the finished simulation waits for the
//    render thread: a = RENDERSUBMIT_START - SIMULATION_END, and backs off as
//    soon as the render thread had to wait for the simulation (b =
//    RENDERSUBMIT_START - previous PRESENT_END).
class FramegenPacer final {
  private:
    struct marks final {
        std::optional<DeviceClock::time_point> simulation_start{};
        std::optional<DeviceClock::time_point> simulation_end{};
        std::optional<DeviceClock::time_point> rendersubmit_start{};
        std::optional<DeviceClock::time_point> present_start{};
        std::optional<DeviceClock::time_point> present_end{};
    };

    std::mutex mutex{};
    std::map<std::uint64_t, marks> frame_marks{};
    std::uint64_t last_used_frame{};

    // The last out-of-band submission of each frame, and when it completed.
    std::map<std::uint64_t, std::shared_ptr<TimestampPool::Handle>>
        out_of_band_last{};
    std::map<std::uint64_t, DeviceClock::time_point> out_of_band_end{};

    // EWMAs.
    DeviceClock::duration simulation_period{};
    DeviceClock::duration gpu_period{};
    DeviceClock::duration simulation_time{};

    double delay_ns{};

  public:
    FramegenPacer();
    FramegenPacer(const FramegenPacer&) = delete;
    FramegenPacer(FramegenPacer&&) = delete;
    FramegenPacer& operator=(const FramegenPacer&) = delete;
    FramegenPacer& operator=(FramegenPacer&&) = delete;
    ~FramegenPacer();

  public:
    void notify_marker(const std::uint64_t frame,
                       const VkLatencyMarkerNV marker);

    void notify_out_of_band_submit(
        const std::uint64_t frame,
        const std::shared_ptr<TimestampPool::Handle>& handle);

    // Called by the swapchain monitor after the normal sleep; sleep_called is
    // when the application called vkLatencySleepNV.
    void hold(const DeviceClock::time_point& sleep_called);
};

} // namespace low_latency

#endif
