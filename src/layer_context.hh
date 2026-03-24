#ifndef LAYER_CONTEXT_HH_
#define LAYER_CONTEXT_HH_

#include <mutex>
#include <unordered_map>
#include <vulkan/vulkan_core.h>

#include "context.hh"
#include "device_context.hh"
#include "instance_context.hh"
#include "physical_device_context.hh"
#include "queue_context.hh"

// The purpose of this file is to provide a definition for the highest level
// entry point struct of our vulkan state.

namespace low_latency {

// All these templates do is make it so we can go from some DispatchableType
// to their respective context's with nice syntax. This lets us write something
// like this for all DispatchableTypes:
//
//     const auto device_context = get_context(some_vk_device);
//           ^ It was automatically deduced as DeviceContext, wow!

template <typename T>
concept DispatchableType =
    std::same_as<std::remove_cvref_t<T>, VkInstance> ||
    std::same_as<std::remove_cvref_t<T>, VkPhysicalDevice> ||
    std::same_as<std::remove_cvref_t<T>, VkDevice> ||
    std::same_as<std::remove_cvref_t<T>, VkQueue>;

template <class D> struct context_for_t;
template <> struct context_for_t<VkInstance> {
    using context = InstanceContext;
};
template <> struct context_for_t<VkPhysicalDevice> {
    using context = PhysicalDeviceContext;
};
template <> struct context_for_t<VkDevice> {
    using context = DeviceContext;
};
template <> struct context_for_t<VkQueue> {
    using context = QueueContext;
};
template <DispatchableType D>
using dispatch_context_t = typename context_for_t<D>::context;

class LayerContext final : public Context {
  private:
    // If this is not null and set to exactly "1", then we should sleep after
    // present.
    static constexpr auto SLEEP_AFTER_PRESENT_ENV =
        "LOW_LATENCY_LAYER_SLEEP_AFTER_PRESENT";

    // If this is not null and set to exactly "1", then VK_NV_LOW_LATENCY2
    // should be provided instead of VK_AMD_anti_lag.
    static constexpr auto SPOOF_NVIDIA_ENV =
        "LOW_LATENCY_LAYER_SPOOF_NV_LOWLATENCY2";

  public:
    std::mutex mutex;
    std::unordered_map<void*, std::shared_ptr<Context>> contexts;

    bool is_antilag_1_enabled = false;
    bool should_spoof_nvidia = false;

  public:
    LayerContext();
    virtual ~LayerContext();

  public:
    template <DispatchableType DT> static void* get_key(const DT& dt) {
        return reinterpret_cast<void*>(dt);
    }

    template <DispatchableType DT>
    std::shared_ptr<dispatch_context_t<DT>> get_context(const DT& dt) {
        const auto key = get_key(dt);

        const auto lock = std::scoped_lock(this->mutex);
        const auto it = this->contexts.find(key);
        assert(it != std::end(this->contexts));

        using context_t = dispatch_context_t<DT>;
        const auto ptr = std::dynamic_pointer_cast<context_t>(it->second);
        assert(ptr);
        return ptr;
    }
};

}; // namespace low_latency

#endif