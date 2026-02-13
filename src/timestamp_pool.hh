#ifndef TIMESTAMP_POOL_HH_
#define TIMESTAMP_POOL_HH_

// The purpose of this file is to provide the definition of a 'timestamp pool'.
// It manages blocks of timestamp query pools, hands them out when requested,
// and allocates more when (if) we run out. It also efficiently reads them back.
// This class solves some key issues:
//
// 1. We need a potentially infinite amount of timestamps available to the
// GPU. While I imagine most (good) applications will limit the amount of
// times they call vkQueueSubmit, there's no bound we can place on the
// amount of times this function will be called. Also,
// the amount of frames in flight might vary, so really we need
// num_queue_submits * max_frames_in_flight timestamps. Obviously, we don't
// know what these numbers are at runtime and can't assume that they are
// reasonable or even constant either. We solve this by allocating more
// timestamps when necessary.

// 2. We don't want to hammer vulkan with expensive timestamp read
// operations. If we have hundreds of query pools lying around, reading them
// back will take hundreds of individual vulkan calls. They
// should be batched as to perform as few reads as possible. So if we allocate
// multiple big query pool strips, then reading them will only require that many
// calls. We then can cache off the result of reading as well so iterating
// through later doesn't require any vulkan interaction at all.
//
//
// Usage:
//     1. Get handle with .acquire().
//     2. Write start/end timestamp operations with the handle's pool and index
//     into the provided command buffer.
//     3. With the command buffer signalled completion via some semaphore /
//     fence, call .poll(). This will cache off all outstanding handles.
//     Retrieving with handles which have not been signalled are undefined.
//     4. Retrieve timestamp results with .get_polled(your_handle).
//     5. Destruct the handle to return the key to the pool.

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan.hpp>

#include <memory>
#include <unordered_set>

namespace low_latency {

class QueueContext;

class TimestampPool final {
  private:
    static constexpr auto TIMESTAMP_QUERY_POOL_SIZE = 512u;
    static_assert(TIMESTAMP_QUERY_POOL_SIZE % 2 == 0);

  private:
    QueueContext& queue_context;

    // VkQueryPool with an unordered set of keys available for reading.
    using available_query_indicies_t = std::unordered_set<std::uint64_t>;

    struct Block {
        VkQueryPool query_pool;
        std::unique_ptr<available_query_indicies_t> available_indicies;
        std::unique_ptr<std::vector<VkCommandBuffer>> command_buffers;
    };
    std::vector<Block> blocks; // multiple blocks

    // A snapshot of all available blocks for reading after each poll.
    std::vector<std::unique_ptr<std::vector<std::uint64_t>>> cached_timestamps;

  public:
    // A handle represents two std::uint64_t blocks oftimestamp memory and two
    // command buffers.
    struct Handle final {
      private:
        friend class TimestampPool;

      private:
        available_query_indicies_t& index_origin;
        const std::size_t block_index;

      public:
        const VkQueryPool query_pool;
        const std::uint64_t query_index;
        const std::array<VkCommandBuffer, 2> command_buffers;

      public:
        Handle(TimestampPool::available_query_indicies_t& index_origin,
               const std::size_t block_index, const VkQueryPool& query_pool,
               const std::uint64_t query_index,
               const std::array<VkCommandBuffer, 2>& command_buffers);
        Handle(const Handle& handle) = delete;
        Handle(Handle&&) = delete;
        Handle operator=(const Handle& handle) = delete;
        Handle operator=(Handle&&) = delete;
        ~Handle(); // frees from the pool

      public:
        void setup_command_buffers(const VkuDeviceDispatchTable& vtable) const;
    };

  private:
    Block allocate();

  public:
    TimestampPool(QueueContext& queue_context);
    TimestampPool(const TimestampPool&) = delete;
    TimestampPool(TimestampPool&&) = delete;
    TimestampPool operator=(const TimestampPool&) = delete;
    TimestampPool operator=(TimestampPool&&) = delete;
    ~TimestampPool();

  public:
    // Hands out a Handle with a pool and index of two uint64_t's.
    std::shared_ptr<Handle> acquire();

    void poll(); // saves the current state for future get's.

    std::uint64_t get_polled(const Handle& handle, const bool hack = false);
};

} // namespace low_latency

#endif