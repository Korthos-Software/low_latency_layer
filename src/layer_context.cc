#include "layer_context.hh"

#include <cstdlib> // for env var
#include <string_view>

namespace low_latency {

LayerContext::LayerContext() {
    const auto parse_bool_env = [](const auto& name) -> bool {
        const auto env = std::getenv(name);
        return env && std::string_view{env} == "1";
    };

    this->should_expose_reflex = parse_bool_env(EXPOSE_REFLEX_ENV);
    this->should_spoof_nvidia = parse_bool_env(SPOOF_NVIDIA_ENV);
}

LayerContext::~LayerContext() {}

} // namespace low_latency