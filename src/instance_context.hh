#ifndef INSTANCE_CONTEXT_HH_
#define INSTANCE_CONTEXT_HH_

#include <vulkan/utility/vk_dispatch_table.h>

#include <memory>
#include <unordered_map>

#include "context.hh"

namespace low_latency {

class LayerContext;
class PhysicalDeviceContext;

class InstanceContext final : public Context {
  public:
    const LayerContext& layer;
    const VkInstance instance{};
    const VkuInstanceDispatchTable vtable{};
    const bool is_simulation_decoupled{};
    const bool should_strict_sync{};

    std::unordered_map<void*, std::shared_ptr<PhysicalDeviceContext>>
        physical_devices{};

  public:
    explicit InstanceContext(const LayerContext& parent_context,
                             const VkInstance& instance,
                             const VkInstanceCreateInfo& create_info,
                             VkuInstanceDispatchTable&& vtable);
    virtual ~InstanceContext();
};

}; // namespace low_latency

#endif