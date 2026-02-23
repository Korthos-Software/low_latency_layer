#ifndef DEVICE_CONTEXT_HH_
#define DEVICE_CONTEXT_HH_

#include <chrono>
#include <deque>
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

    const VkDevice device;
    const VkuDeviceDispatchTable vtable;

    std::unordered_map<VkQueue, std::shared_ptr<QueueContext>> queues;

    // We map swapchains to image indexes and their last signalled semaphore.
    using index_semaphores_t = std::unordered_map<std::uint32_t, VkSemaphore>;
    std::unordered_map<VkSwapchainKHR, index_semaphores_t> swapchain_signals;

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
        void calibrate();
        time_point_t ticks_to_time(const std::uint64_t& ticks) const;
    };
    Clock clock;

    
    std::uint32_t antilag_fps = 0;
    VkAntiLagModeAMD antilag_mode = VK_ANTI_LAG_MODE_DRIVER_CONTROL_AMD;

    // The queue used in the last present.
    std::shared_ptr<QueueContext> present_queue;

  private:
    void sleep_in_input();

  public:
    DeviceContext(InstanceContext& parent_instance,
                  PhysicalDeviceContext& parent_physical,
                  const VkDevice& device, VkuDeviceDispatchTable&& vtable);
    virtual ~DeviceContext();

  public:
    void notify_acquire(const VkSwapchainKHR& swapchain,
                        const std::uint32_t& image_index,
                        const VkSemaphore& signal_semaphore);

    // 
    void notify_antilag_update(const VkAntiLagDataAMD& data);
    
    void notify_queue_present(const QueueContext& queue);
};

}; // namespace low_latency

#endif