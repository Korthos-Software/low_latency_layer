#ifndef TIMESTAMP_POOL_HH_
#define TIMESTAMP_POOL_HH_

// The purpose of this file is to provide the definition of a 'timestamp pool'.
// It manages blocks of timestamp query pools, hands them out when requested,
// and allocates more when (if) we run out.
// Usage:
//     1. Get handle with .acquire().
//     2. Write start/end timestamp operations with the handle's pool and index
//     into the provided command buffer. Will return nullopt if they're
//     not yet available.
//     3. Destruct the handle to return the key to the pool.

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan.hpp>

#include <memory>
#include <unordered_set>
#include <vector>

#include "device_context.hh"

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
        // For our spinlock functions this is the period in which we sleep
        // between attempts.
        static constexpr auto SPINLOCK_MAX_DELAY = std::chrono::microseconds(1);

      private:
        const TimestampPool& timestamp_pool;
        const std::weak_ptr<QueryChunk> origin_chunk;

      public:
        const VkQueryPool query_pool;
        const std::uint64_t query_index;
        const VkCommandBuffer command_buffer;

      public:
        Handle(const TimestampPool& timestamp_pool,
               const std::shared_ptr<QueryChunk>& origin_chunk,
               const std::uint64_t& query_index);
        Handle(const Handle& handle) = delete;
        Handle(Handle&&) = delete;
        Handle operator=(const Handle& handle) = delete;
        Handle operator=(Handle&&) = delete;
        ~Handle();

      public:
        void setup_command_buffers(const Handle& tail,
                                   const QueueContext& queue_context) const;

        // Attempts to get_time, but returns an optional if it's not available
        // yet.
        std::optional<DeviceContext::Clock::time_point_t> get_time();

        // Calls get_time() repeatedly under a spinlock, or gives up at
        // time_point_t and returns std::nullopt.
        std::optional<DeviceContext::Clock::time_point_t>
        get_time_spinlock(const DeviceContext::Clock::time_point_t& until);

        // Calls get_time() repeatedly under a spinlock until it's available.
        DeviceContext::Clock::time_point_t get_time_spinlock();

        // Calls get_time with the assumption it's already available.
        DeviceContext::Clock::time_point_t get_time_required();
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