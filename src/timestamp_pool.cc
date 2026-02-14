#include "timestamp_pool.hh"
#include "device_context.hh"
#include "queue_context.hh"

#include <ranges>
#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan_core.h>

namespace low_latency {

TimestampPool::QueryChunk::QueryChunk(const QueueContext& queue_context) {
    const auto& device_context = queue_context.device_context;
    const auto& vtable = device_context.vtable;

    this->query_pool = [&]() {
        const auto qpci = VkQueryPoolCreateInfo{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = QueryChunk::CHUNK_SIZE};

        auto qp = VkQueryPool{};
        vtable.CreateQueryPool(device_context.device, &qpci, nullptr, &qp);
        return qp;
    }();

    constexpr auto key_range = std::views::iota(0u, QueryChunk::CHUNK_SIZE);
    this->free_indices = std::make_unique<free_indices_t>(std::begin(key_range),
                                                          std::end(key_range));

    this->command_buffers = [&, this]() -> auto {
        auto cbs = std::make_unique<std::vector<VkCommandBuffer>>(CHUNK_SIZE);
        const auto cbai = VkCommandBufferAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = queue_context.command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = static_cast<std::uint32_t>(std::size(*cbs)),
        };
        vtable.AllocateCommandBuffers(device_context.device, &cbai,
                                      std::data(*cbs));
        return cbs;
    }();
}

TimestampPool::QueryChunk::~QueryChunk() {}

TimestampPool::TimestampPool(QueueContext& queue_context)
    : queue_context(queue_context) {

    // Allocate one block on construction, it's likely more than enough.
    auto query_chunk = std::make_shared<QueryChunk>(this->queue_context);
    this->query_chunks.emplace(std::move(query_chunk));
}

std::shared_ptr<TimestampPool::Handle> TimestampPool::acquire() {

    // Gets the empty one, or inserts a new one and returns it.
    const auto not_empty_iter = [this]() -> auto {
        const auto not_empty_iter =
            std::ranges::find_if(this->query_chunks, [](const auto& qc) {
                assert(qc);
                return std::size(*qc->free_indices);
            });

        if (not_empty_iter != std::end(this->query_chunks)) {
            return not_empty_iter;
        }

        const auto insert = std::make_shared<QueryChunk>(this->queue_context);
        const auto [iter, did_insert] = this->query_chunks.emplace(insert);
        assert(did_insert);
        return iter;
    }();

    // Grab any element from our set and erase it immediately after.
    auto& indices = *(*not_empty_iter)->free_indices;
    const auto query_index = *std::begin(indices);
    assert(indices.erase(query_index));

    return std::make_shared<Handle>(*not_empty_iter, query_index);
}

TimestampPool::Handle::Handle(const std::shared_ptr<QueryChunk>& origin_chunk,
                              const std::uint64_t& query_index)
    : query_pool(origin_chunk->query_pool), query_index(query_index),
      origin_chunk(origin_chunk),
      command_buffer((*origin_chunk->command_buffers)[query_index]) {}

TimestampPool::Handle::~Handle() {
    // Parent destructing shouldn't mean we should have a bunch of insertions
    // for zero reason.
    if (const auto ptr = this->origin_chunk.lock(); ptr) {
        assert(ptr->free_indices->insert(this->query_index).second);
    }
}

void TimestampPool::Handle::setup_command_buffers(
    const Handle& tail, const QueueContext& queue_context) const {

    const auto cbbi = VkCommandBufferBeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };

    const auto& device_context = queue_context.device_context;
    const auto& vtable = device_context.vtable;

    vtable.ResetQueryPoolEXT(device_context.device, this->query_pool,
                             this->query_index, 1);

    vtable.BeginCommandBuffer(this->command_buffer, &cbbi);
    vtable.CmdWriteTimestamp2KHR(this->command_buffer,
                                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                 this->query_pool, this->query_index);
    vtable.EndCommandBuffer(this->command_buffer);

    vtable.ResetQueryPoolEXT(device_context.device, tail.query_pool,
                             tail.query_index, 1);
    vtable.ResetCommandBuffer(tail.command_buffer, 0);
    vtable.BeginCommandBuffer(tail.command_buffer, &cbbi);
    vtable.CmdWriteTimestamp2KHR(tail.command_buffer,
                                 VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                                 tail.query_pool, tail.query_index);
    vtable.EndCommandBuffer(tail.command_buffer);
}

std::optional<std::uint64_t>
TimestampPool::Handle::get_ticks(const TimestampPool& pool) {

    const auto& device_context = pool.queue_context.device_context;
    const auto& vtable = device_context.vtable;

    struct QueryResult {
        std::uint64_t value;
        std::uint64_t available;
    };
    auto query_result = QueryResult{};

    const auto r = vtable.GetQueryPoolResults(
        device_context.device, query_pool, this->query_index, 1,
        sizeof(query_result), &query_result, sizeof(query_result),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

    assert(r == VK_SUCCESS || r == VK_NOT_READY);

    if (!query_result.available) {
        return std::nullopt;
    }
    return query_result.value;
}

/*
void TimestampPool::poll() {
    const auto& device_context = this->queue_context.device_context;

    std::ranges::transform(
        this->blocks, std::back_inserter(this->cached_timestamps),
        [&, this](const auto& block) {
            const auto& query_pool = block.query_pool;

            auto timestamps = std::make_unique<std::vector<std::uint64_t>>(
                this->TIMESTAMP_QUERY_POOL_SIZE);

            const auto result = device_context.vtable.GetQueryPoolResults(
                device_context.device, query_pool, 0,
                this->TIMESTAMP_QUERY_POOL_SIZE,
                this->TIMESTAMP_QUERY_POOL_SIZE * sizeof(std::uint64_t),
                std::data(*timestamps), sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT);

            // Might return not ready when any of them aren't ready, which
            // is not an error for our use case.
            assert(result == VK_SUCCESS || result == VK_NOT_READY);

            return timestamps;
        });
};
*/

TimestampPool::~TimestampPool() {
    const auto& device = this->queue_context.device_context.device;
    const auto& vtable = this->queue_context.device_context.vtable;
    for (const auto& query_chunk : this->query_chunks) {
        vtable.FreeCommandBuffers(device, this->queue_context.command_pool,
                                  std::size(*query_chunk->command_buffers),
                                  std::data(*query_chunk->command_buffers));
        vtable.DestroyQueryPool(device, query_chunk->query_pool, nullptr);
    }
}

} // namespace low_latency