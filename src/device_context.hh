#ifndef DEVICE_CONTEXT_HH_
#define DEVICE_CONTEXT_HH_

#include <chrono>
#include <memory>
#include <unordered_map>

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include "context.hh"
#include "instance_context.hh"
#include "physical_device_context.hh"

namespace low_latency {

class QueueContext;

struct DeviceContext final : public Context {
  public:
    InstanceContext& instance;
    PhysicalDeviceContext& physical_device;

    const bool was_antilag_requested;

    const VkDevice device;
    const VkuDeviceDispatchTable vtable;

    // Tiny struct to represent any swapchain's low latency state.
    struct SwapchainInfo {
        std::chrono::milliseconds present_delay = std::chrono::milliseconds{0};
        bool was_low_latency_requested = false;
    };
    std::unordered_map<VkSwapchainKHR, SwapchainInfo> swapchain_infos{};

    std::unordered_map<VkQueue, std::shared_ptr<QueueContext>> queues;

    struct Clock {
      public:
        using time_point_t = std::chrono::time_point<std::chrono::steady_clock,
                                                     std::chrono::nanoseconds>;
        const DeviceContext& device;

      public:
        std::uint64_t host_ns;
        std::uint64_t error_bound;
        std::uint64_t device_ticks;

      public:
        Clock(const DeviceContext& device);
        ~Clock();

      public:
        // WARNING: This *MUST* be used over std::chrono::steady_clock::now if
        // you're planning on comparing it to a device's clock. If it isn't, the
        // timestamps might from different domains and will be completely
        // nonsensical.
        static time_point_t now();

      public:
        void calibrate();
        time_point_t ticks_to_time(const std::uint64_t& ticks) const;
    };
    std::unique_ptr<Clock> clock;

  public:
    DeviceContext(InstanceContext& parent_instance,
                  PhysicalDeviceContext& parent_physical,
                  const VkDevice& device, const bool was_antilag_requested,
                  VkuDeviceDispatchTable&& vtable);
    virtual ~DeviceContext();

  public:
    void sleep_in_input();

    // Updates the settings associated with that swapchain. If none is provided
    // all swapchains are set to this value.
    void update_swapchain_infos(const std::optional<VkSwapchainKHR> target,
                                const std::chrono::milliseconds& present_delay,
                                const bool was_low_latency_requested);
};

}; // namespace low_latency

#endif