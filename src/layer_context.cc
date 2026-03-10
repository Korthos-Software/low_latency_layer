#include "layer_context.hh"

#include <cstdlib> // for env var
#include <string_view>

namespace low_latency {

LayerContext::LayerContext() {
    this->is_antilag_1_enabled = []() -> auto {
        const auto env = std::getenv(LayerContext::SLEEP_AFTER_PRESENT_ENV);
        return env && std::string_view{env} == "1";
    }();
}

LayerContext::~LayerContext() {}

} // namespace low_latency