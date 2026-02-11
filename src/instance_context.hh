#ifndef INSTANCE_CONTEXT_HH_
#define INSTANCE_CONTEXT_HH_

#include <vulkan/utility/vk_dispatch_table.h>

#include <memory>
#include <unordered_map>

#include "context.hh"

namespace low_latency {

class PhysicalDeviceContext;

struct InstanceContext final : public Context {

    const VkInstance instance;
    const VkuInstanceDispatchTable vtable;

    std::unordered_map<void*, std::shared_ptr<PhysicalDeviceContext>> phys_devices;

  public:
    InstanceContext(const VkInstance& instance,
                    VkuInstanceDispatchTable&& vtable);
    virtual ~InstanceContext();
};

}; // namespace low_latency

#endif