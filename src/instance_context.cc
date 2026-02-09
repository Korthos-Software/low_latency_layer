#include "instance_context.hh"

#include <utility>

namespace low_latency {

InstanceContext::InstanceContext(const VkInstance& instance,
                                 VkuInstanceDispatchTable&& vtable)
    : instance(instance), vtable(std::move(vtable)) {}

InstanceContext::~InstanceContext() {}

} // namespace low_latency