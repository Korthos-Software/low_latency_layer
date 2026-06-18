#ifndef DELAY_CONTROLLER_HH_
#define DELAY_CONTROLLER_HH_

#include "device_clock.hh"

#include <mutex>
#include <optional>

namespace low_latency {

// Most games don't allow the simulation thread to run ahead of the render
// thread by more than 1 frame - they're tightly coupled. At least one game
// (Marvel Rivals) decouples the simulation thread from the render thread
// entirely. It seems to allow simulation to run many frames (at least 2-3) in
// the future. This means that, from the GPU and layer side, simply delaying GPU
// completion by the previous frame's gpu completion is actually not enough to
// improve latency sigificantly because it's old data.
//
// Applying a simple constant wait of only 2ms reduced latency in this case
// significantly without reducing throughput (this was at ~140fps). We went from
// around 35ms median latency, to 24ms. This is becase we are forcing the
// simulation thread back to a 0-1 queued frame state, instead of 2-3, by
// artificially increasing simulation time via this artifical wait.
//
// We don't want to apply this extra sleep on games which do not have this issue
// as that would cause a latency regression. I had a look for obvious signals
// like 'cpu time took >2x gpu time', but this isn't reliable for many reasons.
//
//     1. A signal like that is identical to just CPU bound cases. Waiting there
//     would be latency suicide.
//     2. Marvel Rivals doesn't even show this relationship at all. It just
//     looks like a normal, well paced game from our layer's perspective. The
//     GPU and CPU times are truly decoupled. Annoying.
//
// Our solution is to apply a small jitter to every frame. This jitter should
// allow us to tell whether or not we're hitting the simulation floor because
// the actual frametime should increase by that jitter amount. We can derive a
// gradient and push that into an ewma to get a relatively clean signal of
// 'are we there'.
class DelayController {
  private:
    const bool is_simulation_decoupled{};
    std::mutex mutex{};

    struct frame_info final {
        // The distance between the previous frame's release and when we entered
        // delay(). Doesn't include min_delay, jitter, drain or frametime.
        const DeviceClock::time_point::duration frametime{};

        // Jitter to detect if we're at the bottom of the simulation queue.
        const DeviceClock::time_point::duration jitter{};

        // When delay() released the frame.
        const DeviceClock::time_point release{};
    };
    std::optional<frame_info> previous_frame{};

    double gradient_ewma{};
    DeviceClock::duration drain{};

  public:
    explicit DelayController(const bool is_simulation_decoupled);
    DelayController(const DelayController&) = delete;
    DelayController(DelayController&&) = delete;
    DelayController& operator=(const DelayController&) = delete;
    DelayController& operator=(DelayController&&) = delete;
    ~DelayController();

  public:
    void delay(const DeviceClock::duration& min_delay);
};

}; // namespace low_latency

#endif
