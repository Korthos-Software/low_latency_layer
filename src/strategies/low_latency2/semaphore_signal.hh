#ifndef SEMAPHORE_SIGNAL_HH_
#define SEMAPHORE_SIGNAL_HH_

#include "device_context.hh"

#include <cstdint>
#include <vulkan/vulkan.h>

// The VK_NV_low_latency2 extension supports a monotonically increasing
// semaphore value. Monotonically increasing != strictly increasing. We have to
// support a sequence with repeating values like 0, 1, 1, 1, 1, 2, 3. Vulkan
// timeline semaphores do NOT support signalling <= its current value. While the
// frame pacing isn't going to do anything in the case of repeating values (we
// expect a global frame counter), it can happen in the case of swapchain
// recreation or incomplete frames. This tiny class wraps a timeline semaphore
// and associated value. It double checks that we haven't already signalled the
// value.

// TODO we _might_ want to make it so the destructor calls .signal(). This
// would make it impossible to drop semaphores and cause hangs. However,
// there are only a few places this can happen and I want to keep things
// explicit for now.

namespace low_latency {

class SemaphoreSignal {
  private:
    const VkSemaphore semaphore{};
    const std::uint64_t value{};

  public:
    SemaphoreSignal(const VkSemaphore& semaphore, const std::uint64_t& value);
    ~SemaphoreSignal();

  public:
    void signal(const DeviceContext& device_context) const;
};

}; // namespace low_latency

#endif
