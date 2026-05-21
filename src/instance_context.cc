#include "instance_context.hh"

#include "layer_context.hh"

#include <cassert>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace low_latency {

static bool check_known_decoupled_simulation(const VkInstanceCreateInfo& info) {
    // List of applications which are known to have bad behaviour where
    // simulation is allowed to run ahead of render despite the use of
    // reflex/anti-lag.
    // Right now there's only one and I consider it a bug on their part. Reflex
    // or anti_lag extensions should imply 1:1 sim/render.
    static const auto apps =
        std::unordered_set<std::string_view>{"Marvel-Win64-Shipping.exe"};

    if (!info.pApplicationInfo || !info.pApplicationInfo->pApplicationName) {
        return false;
    }

    return apps.contains(info.pApplicationInfo->pApplicationName);
}

static bool check_does_delay_waits(const VkInstanceCreateInfo& info) {
    // List of applications which do not hand off frame pacing to AL2
    // or Reflex, instead injecting their own sleeps regardless.
    static const auto apps = std::unordered_set<std::string_view>{
        "csgo" /* CS2 */, "citadel" /* Deadlock */
    };

    if (!info.pApplicationInfo || !info.pApplicationInfo->pApplicationName) {
        return false;
    }

    return apps.contains(info.pApplicationInfo->pApplicationName);
}

InstanceContext::InstanceContext(const LayerContext& parent_context,
                                 const VkInstance& instance,
                                 const VkInstanceCreateInfo& create_info,
                                 VkuInstanceDispatchTable&& vtable)
    : layer(parent_context), instance(instance), vtable(std::move(vtable)),
      is_simulation_decoupled(parent_context.should_force_decoupled ||
                              check_known_decoupled_simulation(create_info)),
      should_strict_sync(parent_context.should_force_strict_sync ||
                         !check_does_delay_waits(create_info)) {}

InstanceContext::~InstanceContext() {}

} // namespace low_latency