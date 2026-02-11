#include "instance_context.hh"

#include <cassert>
#include <utility>

namespace low_latency {

InstanceContext::InstanceContext(const VkInstance& instance,
                                 VkuInstanceDispatchTable&& vtable)
    : instance(instance), vtable(std::move(vtable)) {}

InstanceContext::~InstanceContext() {
    // Similar to devices, we should own the only shared ptr at this point so
    // they destruct now.
    for (const auto& [device, device_context] : this->phys_devices) {
        assert(device_context.unique());
    }
}

} // namespace low_latency