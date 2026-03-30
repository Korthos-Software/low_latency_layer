#include "timestamp_pool.hh"
#include "device_context.hh"
#include "helper.hh"
#include "queue_context.hh"

#include <mutex>
#include <ranges>
#include <span>
#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan_core.h>

namespace low_latency {

TimestampPool::QueryChunk::QueryPoolOwner::QueryPoolOwner(
    const QueueContext& queue_context)
    : queue_context(queue_context) {

    const auto& device_context = this->queue_context.device;
    const auto qpci =
        VkQueryPoolCreateInfo{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                              .queryType = VK_QUERY_TYPE_TIMESTAMP,
                              .queryCount = QueryChunk::CHUNK_SIZE};

    THROW_NOT_VKSUCCESS(device_context.vtable.CreateQueryPool(
        device_context.device, &qpci, nullptr, &this->query_pool));
}

TimestampPool::QueryChunk::QueryPoolOwner::~QueryPoolOwner() {
    const auto& device_context = this->queue_context.device;
    device_context.vtable.DestroyQueryPool(device_context.device,
                                           this->query_pool, nullptr);
}

TimestampPool::QueryChunk::QueryChunk(const QueueContext& queue_context)
    : query_pool(std::make_unique<QueryPoolOwner>(queue_context)),
      command_buffers(std::make_unique<CommandBuffersOwner>(queue_context)) {

    this->free_indices = []() {
        constexpr auto KEYS = std::views::iota(0u, QueryChunk::CHUNK_SIZE);
        return std::make_unique<free_indices_t>(std::from_range, KEYS);
    }();
}

TimestampPool::QueryChunk::CommandBuffersOwner::CommandBuffersOwner(
    const QueueContext& queue_context)
    : queue_context(queue_context), command_buffers(CHUNK_SIZE) {

    const auto& device_context = queue_context.device;

    const auto cbai = VkCommandBufferAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = *queue_context.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = CHUNK_SIZE,
    };
    THROW_NOT_VKSUCCESS(device_context.vtable.AllocateCommandBuffers(
        device_context.device, &cbai, std::data(this->command_buffers)));
}

TimestampPool::QueryChunk::CommandBuffersOwner::~CommandBuffersOwner() {
    const auto& device_context = this->queue_context.device;

    device_context.vtable.FreeCommandBuffers(
        device_context.device, *this->queue_context.command_pool,
        static_cast<std::uint32_t>(std::size(this->command_buffers)),
        std::data(this->command_buffers));
}

VkCommandBuffer TimestampPool::QueryChunk::CommandBuffersOwner::operator[](
    const std::size_t& i) {

    assert(i < CHUNK_SIZE);
    return this->command_buffers[i];
}

TimestampPool::QueryChunk::~QueryChunk() {}

TimestampPool::TimestampPool(QueueContext& queue_context)
    : queue_context(queue_context) {

    // Allocate one block on construction, it's likely more than enough.
    auto query_chunk = std::make_shared<QueryChunk>(this->queue_context);
    this->query_chunks.emplace(std::move(query_chunk));
}

std::shared_ptr<TimestampPool::Handle> TimestampPool::acquire() {
    const auto lock = std::scoped_lock{this->mutex};

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
    indices.erase(query_index);

    return std::make_shared<Handle>(*this, *not_empty_iter, query_index);
}

TimestampPool::Handle::Handle(TimestampPool& timestamp_pool,
                              const std::shared_ptr<QueryChunk>& origin_chunk,
                              const std::uint64_t& query_index)
    : timestamp_pool(timestamp_pool), origin_chunk(origin_chunk),
      query_pool(*origin_chunk->query_pool), query_index(query_index),
      command_buffer((*origin_chunk->command_buffers)[query_index]) {}

TimestampPool::Handle::~Handle() {
    const auto lock = std::scoped_lock{this->timestamp_pool.mutex};

    // Parent destructing shouldn't mean we should have a bunch of
    // insertions for zero reason.
    if (const auto ptr = this->origin_chunk.lock(); ptr) {
        ptr->free_indices->insert(this->query_index);
    }
}

void TimestampPool::Handle::setup_command_buffers(
    const Handle& tail, const QueueContext& queue_context) const {

    const auto cbbi = VkCommandBufferBeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };

    const auto& device_context = queue_context.device;
    const auto& vtable = device_context.vtable;

    vtable.ResetQueryPoolEXT(device_context.device, this->query_pool,
                             static_cast<std::uint32_t>(this->query_index), 1);

    THROW_NOT_VKSUCCESS(vtable.ResetCommandBuffer(this->command_buffer, 0));
    THROW_NOT_VKSUCCESS(vtable.BeginCommandBuffer(this->command_buffer, &cbbi));

    vtable.CmdWriteTimestamp2KHR(
        this->command_buffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        this->query_pool, static_cast<std::uint32_t>(this->query_index));

    THROW_NOT_VKSUCCESS(vtable.EndCommandBuffer(this->command_buffer));

    vtable.ResetQueryPoolEXT(device_context.device, tail.query_pool,
                             static_cast<std::uint32_t>(tail.query_index), 1);

    THROW_NOT_VKSUCCESS(vtable.ResetCommandBuffer(tail.command_buffer, 0));
    THROW_NOT_VKSUCCESS(vtable.BeginCommandBuffer(tail.command_buffer, &cbbi));

    vtable.CmdWriteTimestamp2KHR(
        tail.command_buffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        tail.query_pool, static_cast<std::uint32_t>(tail.query_index));

    THROW_NOT_VKSUCCESS(vtable.EndCommandBuffer(tail.command_buffer));
}

struct QueryResult {
    std::uint64_t value;
    std::uint64_t available;
};
std::optional<DeviceClock::time_point_t> TimestampPool::Handle::get_time() {
    const auto& context = this->timestamp_pool.queue_context.device;
    const auto& vtable = context.vtable;

    auto query_result = QueryResult{};

    const auto result = vtable.GetQueryPoolResults(
        context.device, query_pool,
        static_cast<std::uint32_t>(this->query_index), 1, sizeof(query_result),
        &query_result, sizeof(query_result),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

    if (result != VK_SUCCESS && result != VK_NOT_READY) {
        throw result;
    }

    if (!query_result.available) {
        return std::nullopt;
    }

    return context.clock->ticks_to_time(query_result.value);
}

DeviceClock::time_point_t TimestampPool::Handle::await_time() {
    const auto& context = this->timestamp_pool.queue_context.device;
    const auto& vtable = context.vtable;

    struct QueryResult {
        std::uint64_t value;
        std::uint64_t available;
    };
    auto query_result = QueryResult{};

    THROW_NOT_VKSUCCESS(vtable.GetQueryPoolResults(
        context.device, query_pool,
        static_cast<std::uint32_t>(this->query_index), 1, sizeof(query_result),
        &query_result, sizeof(query_result),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
            VK_QUERY_RESULT_WAIT_BIT));
    assert(query_result.available);

    return context.clock->ticks_to_time(query_result.value);
}

DeviceClock::time_point_t TimestampPool::Handle::get_time_required() {
    const auto time = this->get_time();
    assert(time.has_value());
    return *time;
}

TimestampPool::~TimestampPool() {}

} // namespace low_latency