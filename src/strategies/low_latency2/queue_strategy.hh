#ifndef STRATEGIES_LOW_LATENCY2_QUEUE_STRATEGY_HH_
#define STRATEGIES_LOW_LATENCY2_QUEUE_STRATEGY_HH_

#include "frame_span.hh"
#include "strategies/queue_strategy.hh"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace low_latency {

class QueueContext;

class LowLatency2QueueStrategy final : public QueueStrategy {
  public:
    static constexpr auto MAX_TRACKED_PRESENTS = 50;

    // Mapping of present_id's to submissions. Grabbed later by the device
    // strategy when we present and actually can associate them to some
    // vkSwapchainKHR.
    std::mutex mutex{};
    std::unordered_map<std::uint64_t, std::unique_ptr<FrameSpan>> frame_spans{};
    std::deque<std::uint64_t> stale_present_ids{};
    std::atomic<bool> is_out_of_band{}; // atomic to avoid lock

  public:
    LowLatency2QueueStrategy(QueueContext& queue);
    virtual ~LowLatency2QueueStrategy();

  public:
    virtual void
    notify_submit(const VkSubmitInfo& submit,
                  std::shared_ptr<TimestampPool::Handle> handle) override;
    virtual void
    notify_submit(const VkSubmitInfo2& submit,
                  std::shared_ptr<TimestampPool::Handle> handle) override;
    virtual void notify_present(const VkPresentInfoKHR& present) override;

  public:
    void notify_out_of_band();
};

} // namespace low_latency

#endif
