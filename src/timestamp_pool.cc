#include "timestamp_pool.hh"
#include "device_context.hh"
#include "helper.hh"
#include "queue_context.hh"

#include <functional>
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
        constexpr auto keys = std::views::iota(0u, QueryChunk::CHUNK_SIZE);
        return std::unordered_set<std::uint64_t>(std::from_range, keys);
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
    : queue_context(queue_context),
      reaper_worker(std::bind_front(&TimestampPool::do_reaper, this)) {}

std::shared_ptr<TimestampPool::Handle> TimestampPool::acquire() {
    const auto lock = std::scoped_lock{this->mutex};

    // Gets the empty one, or inserts a new one and returns it.
    auto& query_chunk = [this]() -> auto& {
        const auto not_empty_iter =
            std::ranges::find_if(this->query_chunks, [](const auto& qc) {
                assert(qc);
                return std::size(qc->free_indices);
            });

        if (not_empty_iter != std::end(this->query_chunks)) {
            return **not_empty_iter;
        }

        const auto [iter, did_insert] = this->query_chunks.emplace(
            std::make_unique<QueryChunk>(this->queue_context));
        assert(did_insert);
        return **iter;
    }();

    // Pull any element from our set to use as our query_index here.
    const auto query_index = *std::begin(query_chunk.free_indices);
    query_chunk.free_indices.erase(query_index);

    // Custom deleter function that puts the handle on our async reaper queue.
    const auto reaper_deleter = [this](Handle* const handle) {
        if (!handle) {
            return;
        }

        const auto lock = std::scoped_lock{this->mutex};
        this->expiring_handles.push_back(handle);
        this->cv.notify_one();
    };

    return std::shared_ptr<Handle>(new Handle(*this, query_chunk, query_index),
                                   reaper_deleter);
}

TimestampPool::Handle::Handle(TimestampPool& timestamp_pool,
                              QueryChunk& query_chunk,
                              const std::uint64_t& query_index)
    : timestamp_pool(timestamp_pool), query_chunk(query_chunk),
      query_pool(*query_chunk.query_pool), query_index(query_index),
      command_buffer((*query_chunk.command_buffers)[query_index]) {}

TimestampPool::Handle::~Handle() {}

void TimestampPool::do_reaper(const std::stop_token stoken) {
    for (;;) {
        auto lock = std::unique_lock{this->mutex};
        this->cv.wait(lock, stoken,
                      [&]() { return !this->expiring_handles.empty(); });

        // Keep going and free everything before destructing.
        if (stoken.stop_requested() && this->expiring_handles.empty()) {
            break;
        }

        const auto handle_ptr = this->expiring_handles.front();
        this->expiring_handles.pop_front();

        // Allow more to go on the queue while we wait for it to finish.
        lock.unlock();
        handle_ptr->await_time();

        // Lock our mutex, allow the queue to use it again and delete it.
        lock.lock();
        handle_ptr->query_chunk.free_indices.insert(handle_ptr->query_index);
        delete handle_ptr;
    }
}

void TimestampPool::Handle::write_command(
    const VkPipelineStageFlagBits2 bit) const {

    const auto cbbi = VkCommandBufferBeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };

    const auto& device_context = this->timestamp_pool.queue_context.device;
    const auto& vtable = device_context.vtable;

    vtable.ResetQueryPoolEXT(device_context.device, this->query_pool,
                             static_cast<std::uint32_t>(this->query_index), 1);

    THROW_NOT_VKSUCCESS(vtable.ResetCommandBuffer(this->command_buffer, 0));
    THROW_NOT_VKSUCCESS(vtable.BeginCommandBuffer(this->command_buffer, &cbbi));

    vtable.CmdWriteTimestamp2KHR(this->command_buffer, bit, this->query_pool,
                                 static_cast<std::uint32_t>(this->query_index));

    THROW_NOT_VKSUCCESS(vtable.EndCommandBuffer(this->command_buffer));
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
        context.device, this->query_pool,
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
        context.device, this->query_pool,
        static_cast<std::uint32_t>(this->query_index), 1, sizeof(query_result),
        &query_result, sizeof(query_result),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
            VK_QUERY_RESULT_WAIT_BIT));
    assert(query_result.available);

    return context.clock->ticks_to_time(query_result.value);
}

TimestampPool::~TimestampPool() {}

} // namespace low_latency