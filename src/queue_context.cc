#include "queue_context.hh"
#include "device_context.hh"
#include "timestamp_pool.hh"

namespace low_latency {

static VkCommandPool
make_command_pool(const DeviceContext& device_context,
                  const std::uint32_t& queue_family_index) {

    const auto cpci = VkCommandPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family_index,
    };

    auto command_pool = VkCommandPool{};
    device_context.vtable.CreateCommandPool(device_context.device, &cpci,
                                            nullptr, &command_pool);
    return command_pool;
}

static VkSemaphore make_semaphore(const DeviceContext& device_context) {

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
    device_context.vtable.CreateSemaphore(device_context.device, &sci, nullptr,
                                          &semaphore);
    return semaphore;
}

QueueContext::QueueContext(DeviceContext& device_context, const VkQueue& queue,
                           const std::uint32_t& queue_family_index)
    : device_context(device_context), queue(queue),
      queue_family_index(queue_family_index),
      // Important we make the command pool before the timestamp pool, because
      // it's a dependency.
      command_pool(make_command_pool(device_context, queue_family_index)),
      semaphore(make_semaphore(device_context)),
      timestamp_pool(std::make_unique<TimestampPool>(*this)) {}

QueueContext::~QueueContext() {
    
    // nuke our handles, so we avoid segfaults for now
    this->handle_hack.clear();
    
    // Ugly - destructors of timestamp_pool should be called before we destroy
    // our vulkan objects.
    this->timestamp_pool.reset();

    const auto& vtable = this->device_context.vtable;
    vtable.DestroySemaphore(this->device_context.device, this->semaphore,
                            nullptr);
    vtable.DestroyCommandPool(this->device_context.device, this->command_pool,
                              nullptr);
}

} // namespace low_latency