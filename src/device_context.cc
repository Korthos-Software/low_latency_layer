#include "device_context.hh"
#include "queue_context.hh"

#include <utility>

namespace low_latency {

DeviceContext::DeviceContext(InstanceContext& parent_instance,
                             const VkDevice& device,
                             VkuDeviceDispatchTable&& vtable)
    : instance(parent_instance), device(device), vtable(std::move(vtable))

{}

} // namespace low_latency