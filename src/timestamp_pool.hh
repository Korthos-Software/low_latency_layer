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
#include <vector>

namespace low_latency {

class QueueContext;

class TimestampPool final {
  private:
    QueueContext& queue_context;

    // A chunk of data which is useful for making timestamp queries.
    // Allows association of an index to a query pool and command buffer.
    // We reuse these when they're released.
    struct QueryChunk final {
      private:
        using free_indices_t = std::unordered_set<std::uint64_t>;
        static constexpr auto CHUNK_SIZE = 512u;

      public:
        VkQueryPool query_pool;
        std::unique_ptr<free_indices_t> free_indices;
        std::unique_ptr<std::vector<VkCommandBuffer>> command_buffers;

      public:
        QueryChunk(const QueueContext& queue_context);
        QueryChunk(const QueryChunk& handle) = delete;
        QueryChunk(QueryChunk&&) = delete;
        QueryChunk operator=(const QueryChunk& handle) = delete;
        QueryChunk operator=(QueryChunk&&) = delete;
        ~QueryChunk();
    };
    std::unordered_set<std::shared_ptr<QueryChunk>> query_chunks;

  public:
    // A handle represents a VkCommandBuffer and a query index.
    // Once the Handle goes out of scope, the query index will be returned
    // to the parent pool.
    struct Handle final {
      private:
        friend class TimestampPool;

      private:
        const std::weak_ptr<QueryChunk> origin_chunk;

      public:
        const VkQueryPool query_pool;
        const std::uint64_t query_index;
        const VkCommandBuffer command_buffer;

      public:
        Handle(const std::shared_ptr<QueryChunk>& origin_chunk,
               const std::uint64_t& query_index);
        Handle(const Handle& handle) = delete;
        Handle(Handle&&) = delete;
        Handle operator=(const Handle& handle) = delete;
        Handle operator=(Handle&&) = delete;
        ~Handle();

      public:
        void setup_command_buffers(const Handle& tail,
                                   const QueueContext& queue_context) const;

        std::optional<std::uint64_t> get_ticks(const TimestampPool& pool);
    };

  public:
    TimestampPool(QueueContext& queue_context);
    TimestampPool(const TimestampPool&) = delete;
    TimestampPool(TimestampPool&&) = delete;
    TimestampPool operator=(const TimestampPool&) = delete;
    TimestampPool operator=(TimestampPool&&) = delete;
    ~TimestampPool();

  public:
    // Hands out a Handle!
    std::shared_ptr<Handle> acquire();
};

} // namespace low_latency

#endif