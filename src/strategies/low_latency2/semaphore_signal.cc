#include "semaphore_signal.hh"

#include "helper.hh"

namespace low_latency {

SemaphoreSignal::SemaphoreSignal(const VkSemaphore& semaphore,
                                 const std::uint64_t& value)
    : semaphore(semaphore), value(value) {}

SemaphoreSignal::~SemaphoreSignal() {}

void SemaphoreSignal::signal(const DeviceContext& device) const {

    auto current = std::uint64_t{};
    THROW_NOT_VKSUCCESS(device.vtable.GetSemaphoreCounterValue(
        device.device, this->semaphore, &current));

    // Don't signal if it has already been signalled.
    if (current >= this->value) {
        return;
    }

    const auto ssi =
        VkSemaphoreSignalInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                              .semaphore = this->semaphore,
                              .value = this->value};
    THROW_NOT_VKSUCCESS(device.vtable.SignalSemaphore(device.device, &ssi));
}

} // namespace low_latency