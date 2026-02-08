#include "queue_context.hh"

namespace low_latency {

static VkCommandPool make_command_pool(const VkDevice& device,
                                       const std::uint32_t& queue_family_index,
                                       const VkuDeviceDispatchTable& vtable) {

    const auto cpci = VkCommandPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family_index,
    };

    auto command_pool = VkCommandPool{};
    vtable.CreateCommandPool(device, &cpci, nullptr, &command_pool);
    return command_pool;
}

QueueContext::QueueContext(const VkDevice& device, const VkQueue queue,
                           const std::uint32_t& queue_family_index,
                           const VkuDeviceDispatchTable& vtable)
    : device(device), queue(queue), queue_family_index(queue_family_index),
      vtable(vtable),
      // Important we make the command pool before the timestamp pool, because it's a dependency.
      command_pool(make_command_pool(device, queue_family_index, vtable)),
      timestamp_pool(device, vtable, command_pool) {

    this->semaphore = [&]() -> VkSemaphore {
        const auto stci = VkSemaphoreTypeCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };

        const auto sci = VkSemaphoreCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &stci,
        };

        auto semaphore = VkSemaphore{};
        vtable.CreateSemaphore(device, &sci, nullptr, &semaphore);
        return semaphore;
    }();
}

QueueContext::~QueueContext() {
}

} // namespace low_latency