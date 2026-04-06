#ifndef TIMESTAMP_POOL_HH_
#define TIMESTAMP_POOL_HH_

// The purpose of this file is to provide the definition of a 'timestamp pool'.

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan.hpp>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "device_clock.hh"

namespace low_latency {

class QueueContext;
class DeviceContext;

// A timestamp pool manages blocks of timestamp query pools, hands them out when
// requested, and allocates more when (if) we run out. It _should_ be thread
// safe.
// Usage:
//     1. Get handle with .acquire().
//     2. Write start/end timestamp operations with the handle's pool and index
//        into the provided command buffer.
//     3. Grab the time, or wait until it's ready, using get_time or await_time
//        respectively.
//     4. Destruct the handle to return the key to the pool. The pool handles,
//        via an async reaper thread, when the actual handle's contents can be
//        reused as they must be alive until vulkan is done with them.
class TimestampPool final {
  private:
    QueueContext& queue_context;

    // A chunk of data which is useful for making timestamp queries.
    // Allows association of an index to a query pool and command buffer.
    // We reuse these when they're released.
    class QueryChunk final {
        friend class TimestampPool;

      private:
        static constexpr auto CHUNK_SIZE = 512u;
        // Should be even because we take two each time in our handles.
        static_assert(CHUNK_SIZE % 2 == 0);

      private:
        struct QueryPoolOwner final {
          private:
            const QueueContext& queue_context;
            VkQueryPool query_pool;

          public:
            QueryPoolOwner(const QueueContext& queue_context);
            QueryPoolOwner(const QueryPoolOwner&) = delete;
            QueryPoolOwner(QueryPoolOwner&&) = delete;
            QueryPoolOwner operator=(const QueryPoolOwner&) = delete;
            QueryPoolOwner operator=(QueryPoolOwner&&) = delete;
            ~QueryPoolOwner();

          public:
            operator const VkQueryPool&() const { return this->query_pool; }
        };

        struct CommandBuffersOwner final {
          public:
            const QueueContext& queue_context;
            std::vector<VkCommandBuffer> command_buffers;

          public:
            CommandBuffersOwner(const QueueContext& queue_context);
            CommandBuffersOwner(const CommandBuffersOwner&) = delete;
            CommandBuffersOwner(CommandBuffersOwner&&) = delete;
            CommandBuffersOwner operator=(const CommandBuffersOwner&) = delete;
            CommandBuffersOwner operator=(CommandBuffersOwner&&) = delete;
            ~CommandBuffersOwner();
        };

        std::unique_ptr<QueryPoolOwner> query_pool;
        std::unique_ptr<CommandBuffersOwner> command_buffers;
        // A set of indices which are currently availabe in this chunk.
        std::unordered_set<std::uint32_t> free_indices;

      public:
        QueryChunk(const QueueContext& queue_context);
        QueryChunk(const QueryChunk& handle) = delete;
        QueryChunk(QueryChunk&&) = delete;
        QueryChunk operator=(const QueryChunk& handle) = delete;
        QueryChunk operator=(QueryChunk&&) = delete;
        ~QueryChunk();
    };

  public:
    // A handle represents a VkCommandBuffer and a query index.
    // It represents represents and provides both a start and end command
    // buffer, which can attach start/end timing information to submissions.
    // Once the Handle destructs the query index will be returned to the parent
    // pool - but crucially only when Vulkan is done with it.
    struct Handle final {
      private:
        friend class TimestampPool;

      private:
        TimestampPool& timestamp_pool;
        QueryChunk& query_chunk;

      public:
        const std::uint32_t query_index;

      public:
        Handle(TimestampPool& timestamp_pool, QueryChunk& query_chunk,
               const std::uint32_t query_index);
        Handle(const Handle& handle) = delete;
        Handle operator=(const Handle& handle) = delete;
        Handle(Handle&&) = delete;
        Handle& operator=(Handle&&) = delete;
        ~Handle();

      public:
        const VkCommandBuffer& get_start_buffer() const;
        const VkCommandBuffer& get_end_buffer() const;

      private:
        DeviceClock::time_point_t
        await_time_impl(const std::uint32_t offset) const;

      public:
        // Waits until the time is available and returns it.
        DeviceClock::time_point_t await_start_time() const;
        DeviceClock::time_point_t await_end_time() const;
    };

  private:
    void do_reaper(const std::stop_token stoken);

  private:
    std::deque<Handle*> expiring_handles;
    std::unordered_set<std::unique_ptr<QueryChunk>> query_chunks;

    std::mutex mutex;
    std::condition_variable_any cv;

    std::jthread reaper_worker;

  public:
    TimestampPool(QueueContext& queue_context);
    TimestampPool(const TimestampPool&) = delete;
    TimestampPool(TimestampPool&&) = delete;
    TimestampPool operator=(const TimestampPool&) = delete;
    TimestampPool operator=(TimestampPool&&) = delete;
    ~TimestampPool();

  public:
    std::shared_ptr<Handle> acquire();
};

} // namespace low_latency

#endif