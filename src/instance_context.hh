#ifndef INSTANCE_CONTEXT_HH_
#define INSTANCE_CONTEXT_HH_

#include <vulkan/utility/vk_dispatch_table.h>

namespace low_latency {

struct InstanceContext {

    const VkInstance instance;
    const VkuInstanceDispatchTable vtable;

  public:
    InstanceContext(const VkInstance& instance,
                    VkuInstanceDispatchTable&& vtable);
    InstanceContext(const InstanceContext&) = delete;
    InstanceContext(InstanceContext&&) = delete;
    InstanceContext operator==(const InstanceContext&) = delete;
    InstanceContext operator==(InstanceContext&&) = delete;
    ~InstanceContext();
};

}; // namespace low_latency

#endif