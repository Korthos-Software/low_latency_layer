#include "delay_controller.hh"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <time.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace low_latency {

namespace {

using namespace std::chrono_literals;

static bool env_enabled(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v)
        return false;
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' ||
           v[0] == 'T';
}

static long long env_ll(const char* name, long long fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v)
        return fallback;
    char* end = nullptr;
    const long long parsed = std::strtoll(v, &end, 10);
    if (!end || *end != '\0')
        return fallback;
    return parsed;
}

// Issue a single CPU relaxation hint.
// On x86/x86_64 this is the PAUSE instruction which reduces power consumption
// and avoids memory-order violations in the spin loop.
// On aarch64/arm the YIELD instruction provides the same hint.
static void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

static DeviceClock::duration us_duration(long long us) {
    return std::chrono::duration_cast<DeviceClock::duration>(
        std::chrono::microseconds{us});
}

static DeviceClock::duration abs_duration(DeviceClock::duration d) {
    return d < DeviceClock::duration::zero() ? -d : d;
}

static DeviceClock::duration min_dur(DeviceClock::duration a,
                                     DeviceClock::duration b) {
    return a < b ? a : b;
}

static DeviceClock::duration max_dur(DeviceClock::duration a,
                                     DeviceClock::duration b) {
    return a > b ? a : b;
}

static DeviceClock::duration clamp_dur(DeviceClock::duration v,
                                       DeviceClock::duration lo,
                                       DeviceClock::duration hi) {
    return max_dur(lo, min_dur(v, hi));
}

// Precise hybrid wait: sleeps via clock_nanosleep(TIMER_ABSTIME) until we are
// within spin_window of the target, then busy-spins with cpu_relax().
//
// Using TIMER_ABSTIME avoids accumulated error from repeated TIMER_RELTIME
// calls and avoids the double-conversion overhead of sleep_until with a custom
// clock type.
static void wait_until(DeviceClock::time_point target,
                       DeviceClock::duration spin_window) {
    spin_window = max_dur(spin_window, DeviceClock::duration::zero());
    const auto spin_start = target - spin_window;

    while (true) {
        const auto now = DeviceClock::now();
        if (now >= target)
            return;

        if (now < spin_start) {
            // Sleep until we enter the spin window.
            const auto wake_ns = spin_start.time_since_epoch().count();
            const auto ts = timespec{
                .tv_sec = wake_ns / 1'000'000'000LL,
                .tv_nsec = wake_ns % 1'000'000'000LL,
            };
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
            // Re-evaluate: OS may have woken us slightly late.
            continue;
        }

        // Final precision spin.
        while (DeviceClock::now() < target) {
            cpu_relax();
        }
        return;
    }
}

// Env-var settings, evaluated once per process lifetime via a static local.
//
// LOW_LATENCY_LAYER_DISABLE_DELAY=1
//   Disable all userspace waiting in this controller.
//
// LOW_LATENCY_LAYER_NET_SAFE=1 | LOW_LATENCY_LAYER_DISABLE_DECOUPLED_DELAY=1
//   Disable extra drain for decoupled-simulation games (e.g. Marvel Rivals).
//   Use this if the extra delay is inflating network-tick latency.
//
// LOW_LATENCY_LAYER_MAX_EXTRA_DELAY_US=N  (default: 750)
//   Hard cap in microseconds on how much extra drain can accumulate.
//
// LOW_LATENCY_LAYER_SPIN_US=N             (default: 150)
//   Busy-spin window in microseconds used during the final wait phase.
struct Settings {
    bool disable_all_delay;
    bool net_safe;
    DeviceClock::duration max_extra_delay;
    DeviceClock::duration spin_window;

    static const Settings& get() {
        static const Settings s{
            .disable_all_delay =
                env_enabled("LOW_LATENCY_LAYER_DISABLE_DELAY"),
            .net_safe =
                env_enabled("LOW_LATENCY_LAYER_NET_SAFE") ||
                env_enabled("LOW_LATENCY_LAYER_DISABLE_DECOUPLED_DELAY"),
            .max_extra_delay = us_duration(
                env_ll("LOW_LATENCY_LAYER_MAX_EXTRA_DELAY_US", 750)),
            .spin_window =
                us_duration(env_ll("LOW_LATENCY_LAYER_SPIN_US", 150)),
        };
        return s;
    }
};

} // namespace

DelayController::DelayController(const bool is_simulation_decoupled)
    : is_simulation_decoupled(is_simulation_decoupled) {}

DelayController::~DelayController() {}

void DelayController::delay(const DeviceClock::duration& min_delay) {
    using namespace std::chrono;

    const auto& cfg = Settings::get();
    const auto now0 = DeviceClock::now();

    if (!this->previous_frame.has_value()) {
        this->previous_frame.emplace(frame_info{
            .release = now0,
        });
        return;
    }

    const auto last_release = this->previous_frame->release;
    const auto frametime = now0 - last_release;

    if (cfg.disable_all_delay) {
        this->drain = 0ns;
        this->gradient_ewma = 0.0;
        this->previous_frame.emplace(frame_info{
            .frametime = frametime,
            .jitter = 0ns,
            .release = DeviceClock::now(),
        });
        return;
    }

    // Honor the external min_delay (frame pacing) contract using a precise
    // hybrid wait instead of a bare yield loop.
    if (min_delay != 0ns) {
        wait_until(last_release + min_delay, cfg.spin_window);
    }

    // Coupled simulation/render, or user-requested net-safe mode:
    // skip extra delay entirely so we can't inflate sim-queue or network
    // latency.
    if (!this->is_simulation_decoupled || cfg.net_safe) {
        this->drain = 0ns;
        this->gradient_ewma = 0.0;
        this->previous_frame.emplace(frame_info{
            .frametime = frametime,
            .jitter = 0ns,
            .release = DeviceClock::now(),
        });
        return;
    }

    // Decoupled simulation/render path (e.g. Marvel Rivals).
    //
    // Strategy: additive increase / multiplicative decrease (AIMD).
    //
    //   - Each stable frame adds +25us up to a hard cap = min(frametime/8,
    //     MAX_EXTRA_DELAY_US). At 120fps that cap is ~1ms; at 60fps ~2ms.
    //     This nudges the sim queue toward depth-0/1 while keeping the added
    //     latency far below a full frame.
    //
    //   - If frametime swings >10% vs the previous frame we halve drain
    //     immediately, so a CPU spike or resolution change can't lock in a
    //     large penalty.
    //
    // The old jitter-probe/gradient/ewma approach was noisy, could accumulate
    // drain up to a full frametime, and had sign-inversion issues with Marvel
    // Rivals. The bounded AIMD approach is simpler and safer.

    const auto safe_cap = clamp_dur(
        min_dur(frametime > 0ns ? frametime / 8 : 0ns, cfg.max_extra_delay),
        0ns, cfg.max_extra_delay);

    // Clamp any existing drain to the (possibly tighter) new cap.
    this->drain = clamp_dur(this->drain, 0ns, safe_cap);

    if (this->drain > 0ns) {
        wait_until(DeviceClock::now() + this->drain, cfg.spin_window);
    }

    const auto previous_ft = this->previous_frame->frametime;
    const auto ft_delta =
        previous_ft == 0ns ? 0ns : abs_duration(frametime - previous_ft);

    if (frametime > 0ns && ft_delta > frametime / 10) {
        // Unstable frametime: decay aggressively.
        this->drain = this->drain / 2;
    } else {
        // Stable frame: slow additive increase.
        this->drain = min_dur(this->drain + 25us, safe_cap);
    }

    this->previous_frame.emplace(frame_info{
        .frametime = frametime,
        .jitter = 0ns,
        .release = DeviceClock::now(),
    });
}

} // namespace low_latency
