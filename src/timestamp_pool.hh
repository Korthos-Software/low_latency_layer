#ifndef TIMESTAMP_POOL_HH_
#define TIMESTAMP_POOL_HH_

// The purpose of this file is to provide the definition of a 'timestamp pool'.

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

#include "device_clock.hh"

namespace low_latency {

class QueueContext;
class DeviceContext;

// A timestamp pool manages blocks of timestamp query pools, hands them out when
// requested, and allocates more when (if) we run out. It _should_ be thread
// safe. Usage:
//     1. Get handle with .acquire().
//     2. Use its provided head/tail command buffers in a queue submission.
//        These commands write timing information for TOP_OF_PIPE_BIT and
//        BOTTOM_OF_PIPE_BIT respectively.
//     3. If the queue submission succeeds, set was_submitted to true on the
//        handle. This tells the destructor that it needs to wait until the
//        timestamp is written before allowing reuse.
//     4. Grab the time, or wait until it's ready.
//     5. Destruct the handle to return the key to the pool. The pool handles
//        when the actual handle's contents can be reused as they must be alive
//        until vulkan is done with them.
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
            VkQueryPool query_pool{};

          public:
            explicit QueryPoolOwner(const QueueContext& queue_context);
            QueryPoolOwner(const QueryPoolOwner&) = delete;
            QueryPoolOwner(QueryPoolOwner&&) = delete;
            QueryPoolOwner& operator=(const QueryPoolOwner&) = delete;
            QueryPoolOwner& operator=(QueryPoolOwner&&) = delete;
            ~QueryPoolOwner();

          public:
            operator const VkQueryPool&() const { return this->query_pool; }
        };

        struct CommandBuffersOwner final {
          public:
            const QueueContext& queue_context;
            std::vector<VkCommandBuffer> command_buffers{};

          public:
            explicit CommandBuffersOwner(const QueueContext& queue_context);
            CommandBuffersOwner(const CommandBuffersOwner&) = delete;
            CommandBuffersOwner(CommandBuffersOwner&&) = delete;
            CommandBuffersOwner& operator=(const CommandBuffersOwner&) = delete;
            CommandBuffersOwner& operator=(CommandBuffersOwner&&) = delete;
            ~CommandBuffersOwner();
        };

        std::unique_ptr<QueryPoolOwner> query_pool{};
        std::unique_ptr<CommandBuffersOwner> command_buffers{};
        // A set of indices which are currently availabe in this chunk.
        std::unordered_set<std::uint32_t> free_indices{};

      public:
        explicit QueryChunk(const QueueContext& queue_context);
        QueryChunk(const QueryChunk& handle) = delete;
        QueryChunk(QueryChunk&&) = delete;
        QueryChunk& operator=(const QueryChunk& handle) = delete;
        QueryChunk& operator=(QueryChunk&&) = delete;
        ~QueryChunk();
    };

  public:
    // A handle represents a VkCommandBuffer and a query index.
    // It provides both a start and end command buffer, which can attach
    // start/end timing information to submissions. Once the Handle destructs
    // the query index will be returned to the parent pool - but crucially only
    // when Vulkan is done with it.
    struct Handle final {
      private:
        friend class TimestampPool;

      private:
        TimestampPool& timestamp_pool;
        QueryChunk& query_chunk;

      public:
        const std::uint32_t query_index{};
        // If a queue submit allocates a handle, but fails before this is set,
        // then the reaper must skip await_end as the queries were reset but
        // never written (avoiding a hang).
        std::atomic<bool> was_submitted{};

      public:
        explicit Handle(TimestampPool& timestamp_pool, QueryChunk& query_chunk,
                        const std::uint32_t query_index);
        Handle(const Handle& handle) = delete;
        Handle(Handle&&) = delete;
        Handle& operator=(Handle&&) = delete;
        Handle& operator=(const Handle& handle) = delete;
        ~Handle();

      public:
        const VkCommandBuffer& get_start_buffer() const;
        const VkCommandBuffer& get_end_buffer() const;

      private:
        DeviceClock::time_point
        await_time_impl(const std::uint32_t offset) const;

      public:
        // Blocks until the time is available.
        DeviceClock::time_point await_start() const;
        DeviceClock::time_point await_end() const;

      private:
        std::optional<std::uint64_t>
        has_time_impl(const std::uint32_t offset) const;

      public:
        // Checks if the time is available - doesn't block.
        bool has_start() const;
        bool has_end() const;

        // The end time if available - doesn't block.
        std::optional<DeviceClock::time_point> try_end() const;
    };

  private:
    void do_reaper(const std::stop_token stoken);

  private:
    std::mutex mutex{};
    std::condition_variable_any cv{};

    std::deque<Handle*> expiring_handles{};
    std::unordered_set<std::unique_ptr<QueryChunk>> query_chunks{};

    std::jthread reaper_worker{};

  public:
    explicit TimestampPool(QueueContext& queue_context);
    TimestampPool(const TimestampPool&) = delete;
    TimestampPool(TimestampPool&&) = delete;
    TimestampPool& operator=(const TimestampPool&) = delete;
    TimestampPool& operator=(TimestampPool&&) = delete;
    ~TimestampPool();

  public:
    std::shared_ptr<Handle> acquire();
};

} // namespace low_latency

#endif