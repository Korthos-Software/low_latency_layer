#ifndef LAYER_CONTEXT_HH_
#define LAYER_CONTEXT_HH_

#include <shared_mutex>
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
    // VK_NV_low_latency2 should be provided instead of VK_AMD_anti_lag.
    static constexpr auto REFLEX_ENV = "LOW_LATENCY_LAYER_REFLEX";

    // The card's vendor, id, and device name will be modified to appear as a
    // NVIDIA card.
    static constexpr auto SPOOF_NVIDIA_ENV = "LOW_LATENCY_LAYER_SPOOF_NVIDIA";

    // Additional delays for decoupled simulation should be forced on
    // (delay_controller). This is usually automatically handled on a
    // per-application basis.
    static constexpr auto FORCE_DECOUPLED_ENV =
        "LOW_LATENCY_LAYER_FORCE_DECOUPLED";

    // Some applications (CS2) inject their own delays into the pipeline despite
    // asking us to do waiting for them when an in-engine framelimit is active.
    // This causes issues with frame pacing and requires mitigation where we
    // kick off work at the start of the previous frame's work instead of the
    // usual end. This eliminates queueing but isn't absolutely optimal for
    // latency - but neither is using a frame limit in the first place.
    static constexpr auto FORCE_STRICT_SYNC_ENV =
        "LOW_LATENCY_LAYER_FORCE_STRICT_SYNC";

  public:
    // Constants for spoofing.
    static constexpr auto NVIDIA_VENDOR_ID = 0x10DE;
    static constexpr auto NVIDIA_DEVICE_ID = 0x2B85; // 5090
    static constexpr auto NVIDIA_DEVICE_NAME = "NVIDIA GeForce RTX 5090";

  public:
    const bool should_expose_reflex{};
    const bool should_spoof_nvidia{};
    const bool should_force_decoupled{};
    const bool should_force_strict_sync{};

    std::shared_mutex mutex{};
    std::unordered_map<void*, std::shared_ptr<Context>> contexts{};

  public:
    explicit LayerContext();
    virtual ~LayerContext();

  public:
    template <DispatchableType DT> static void* get_key(const DT& dt) {
        return reinterpret_cast<void*>(dt);
    }

    template <DispatchableType DT>
    std::shared_ptr<dispatch_context_t<DT>> get_context(const DT& dt) {
        const auto key = get_key(dt);

        const auto lock = std::shared_lock{this->mutex};
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