#include "instance_context.hh"

#include <cassert>
#include <utility>

namespace low_latency {

InstanceContext::InstanceContext(const LayerContext& parent_context,
                                 const VkInstance& instance,
                                 VkuInstanceDispatchTable&& vtable)
    : layer(parent_context), instance(instance), vtable(std::move(vtable)) {}

InstanceContext::~InstanceContext() {}

} // namespace low_latency