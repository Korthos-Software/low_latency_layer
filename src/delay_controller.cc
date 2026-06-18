#include "delay_controller.hh"

#include <algorithm>

namespace low_latency {

DelayController::DelayController(const bool is_simulation_decoupled)
    : is_simulation_decoupled(is_simulation_decoupled) {}

DelayController::~DelayController() {}

void DelayController::delay(const DeviceClock::duration& min_delay) {
    using namespace std::chrono;
    auto lock = std::unique_lock{this->mutex};

    if (!this->previous_frame.has_value()) {
        this->previous_frame.emplace(frame_info{
            .release = DeviceClock::now(),
        });
        return;
    }

    const auto frametime = DeviceClock::now() - this->previous_frame->release;
    const auto prev_release = this->previous_frame->release;

    // Apply frame limits if nonzero min_delay.
    if (min_delay != 0ns) {
        lock.unlock();
        while (DeviceClock::now() < prev_release + min_delay) {
            std::this_thread::yield();
        }
        lock.lock();
    }

    // Early out and don't apply jitter or delays if we know the application has
    // coupled simulation and render.
    if (!this->is_simulation_decoupled) {
        this->previous_frame.emplace(frame_info{
            .frametime = frametime,
            .release = DeviceClock::now(),
        });
        return;
    }

    // Apply jitter and drain.
    const auto prev_jitter = this->previous_frame->jitter;
    const auto prev_frametime = this->previous_frame->frametime;
    const auto jitter = prev_jitter == 0ns ? frametime / 50 : 0ns;
    const auto drain = this->drain;

    lock.unlock();
    for (const auto start = DeviceClock::now();
         DeviceClock::now() < start + jitter + drain;) {

        std::this_thread::yield();
    }
    lock.lock();

    // For some reason using the UP and DOWN gradient of jitter in Marvel
    // Rivals is causing issues where the expected sign is inverted. This breaks
    // our EWMA signal, so only update it on the upside (there's two sides to
    // every jitter).
    if (prev_jitter == 0ns) {
        this->previous_frame.emplace(frame_info{
            .frametime = frametime,
            .jitter = jitter,
            .release = DeviceClock::now(),
        });
        return;
    }

    // Calculate our gradient.
    // -1 => Applying a random jitter sleep actually improved frametimes.
    //       Simulation depth is probably completely backlogged.
    // 0  => Applying a sleep did nothing. We are behind in simulation depth.
    // 1  => Applying a sleep directly impacted frametimes. Don't sleep!
    const auto gradient = [&]() -> auto {
        const auto dt_jitter = -prev_jitter;
        const auto dt_frametime = frametime - prev_frametime;
        const auto dt_frametime_ns = static_cast<double>(dt_frametime.count());
        const auto dt_jitter_ns = static_cast<double>(dt_jitter.count());
        return std::clamp(dt_frametime_ns / dt_jitter_ns, -1.0, 1.0);
    }();

    // Feed our gradient into ewma -> our gradient is noisy and an ewma smooths
    // it out into a readable signal.
    constexpr auto ALPHA = 0.01;
    this->gradient_ewma =
        (ALPHA * gradient) + ((1.0 - ALPHA) * this->gradient_ewma);

    // Update drain - this should naturally center around 0.5 ewma.
    const auto delta = [&]() -> auto {
        constexpr auto DELTA_GAIN = 5.0;
        const auto ns = prev_jitter.count();
        const auto dt = (0.5 - this->gradient_ewma) * static_cast<double>(ns);
        return DeviceClock::duration{static_cast<long long>(DELTA_GAIN * dt)};
    }();
    this->drain = std::clamp(this->drain + delta, 0ns, frametime);

    this->previous_frame.emplace(frame_info{
        .frametime = frametime,
        .jitter = jitter,
        .release = DeviceClock::now(),
    });
}

} // namespace low_latency
