#include "layer_context.hh"

#include <cstdlib> // for env var
#include <string_view>

namespace low_latency {

static bool parse_bool_env(const auto& name) {
    const auto env = std::getenv(name);
    return env && std::string_view{env} == "1";
}

LayerContext::LayerContext()
    : should_expose_reflex(parse_bool_env(REFLEX_ENV)),
      should_spoof_nvidia(parse_bool_env(SPOOF_NVIDIA_ENV)),
      should_force_decoupled(parse_bool_env(FORCE_DECOUPLED_ENV)),
      should_force_strict_sync(parse_bool_env(FORCE_STRICT_SYNC_ENV)) {}

LayerContext::~LayerContext() {}

} // namespace low_latency