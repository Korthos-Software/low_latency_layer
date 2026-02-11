#include "device_context.hh"
#include "queue_context.hh"

#include <utility>

namespace low_latency {

DeviceContext::DeviceContext(InstanceContext& parent_instance,
                             const VkDevice& device,
                             const PFN_vkSetDeviceLoaderData& sdld,
                             VkuDeviceDispatchTable&& vtable)
    : instance(parent_instance), device(device), sdld(sdld),
      vtable(std::move(vtable)) {}

DeviceContext::~DeviceContext() {
    // We will let the destructor handle clearing here, but they should be
    // unique by now (ie, removed from the layer's context map).
    for (const auto& [queue, queue_context] : this->queues) {
        assert(queue_context.unique());
    }
}

} // namespace low_latency