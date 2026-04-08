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
        constexpr auto keys = std::views::iota(0u, QueryChunk::CHUNK_SIZE) |
                              std::views::stride(2u);
        return std::unordered_set<std::uint32_t>(std::from_range, keys);
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
                              const std::uint32_t query_index)
    : timestamp_pool(timestamp_pool), query_chunk(query_chunk),
      query_index(query_index) {

    const auto cbbi = VkCommandBufferBeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

    const auto& device_context = this->timestamp_pool.queue_context.device;
    const auto& vtable = device_context.vtable;
    const auto command_buffers =
        std::data(this->query_chunk.command_buffers->command_buffers);

    const auto rewrite_cmd = [&](const std::uint32_t offset,
                                 const VkPipelineStageFlagBits2 bit) {
        const auto& command_buffer = command_buffers[query_index + offset];
        const auto& query_pool = *this->query_chunk.query_pool;
        const auto index =
            static_cast<std::uint32_t>(this->query_index) + offset;
        vtable.ResetQueryPoolEXT(device_context.device, query_pool, index, 1);
        THROW_NOT_VKSUCCESS(vtable.ResetCommandBuffer(command_buffer, 0));
        THROW_NOT_VKSUCCESS(vtable.BeginCommandBuffer(command_buffer, &cbbi));
        vtable.CmdWriteTimestamp2KHR(command_buffer, bit, query_pool, index);
        THROW_NOT_VKSUCCESS(vtable.EndCommandBuffer(command_buffer));
    };

    rewrite_cmd(0, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    rewrite_cmd(1, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
}

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
        handle_ptr->await_end();

        // Lock our mutex, allow the queue to use it again and delete it.
        lock.lock();
        handle_ptr->query_chunk.free_indices.insert(handle_ptr->query_index);
        delete handle_ptr;
    }
}

const VkCommandBuffer& TimestampPool::Handle::get_start_buffer() const {
    const auto command_buffers =
        std::data(this->query_chunk.command_buffers->command_buffers);
    return command_buffers[this->query_index];
}

const VkCommandBuffer& TimestampPool::Handle::get_end_buffer() const {
    const auto command_buffers =
        std::data(this->query_chunk.command_buffers->command_buffers);
    return command_buffers[this->query_index + 1];
}

std::uint64_t
TimestampPool::Handle::await_time_impl(const std::uint32_t offset) const {

    const auto& context = this->timestamp_pool.queue_context.device;
    const auto& vtable = context.vtable;
    const auto& query_pool = *this->query_chunk.query_pool;

    auto query_result = std::array<std::uint64_t, 2>{};

    THROW_NOT_VKSUCCESS(vtable.GetQueryPoolResults(
        context.device, query_pool, this->query_index + offset, 1,
        sizeof(query_result), &query_result, sizeof(query_result),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
            VK_QUERY_RESULT_WAIT_BIT));
    assert(query_result[1]);

    return query_result[0];
}

void TimestampPool::Handle::await_start() const { this->await_time_impl(0); }
void TimestampPool::Handle::await_end() const { this->await_time_impl(1); }

std::optional<std::uint64_t>
TimestampPool::Handle::has_time_impl(const std::uint32_t offset) const {

    const auto& context = this->timestamp_pool.queue_context.device;
    const auto& vtable = context.vtable;
    const auto& query_pool = *this->query_chunk.query_pool;

    auto query_result = std::array<std::uint64_t, 2>{};

    const auto result = vtable.GetQueryPoolResults(
        context.device, query_pool, this->query_index + offset, 1,
        sizeof(query_result), &query_result, sizeof(query_result),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

    if (result != VK_NOT_READY && result != VK_SUCCESS) {
        throw result;
    }

    if (!query_result[1]) {
        return std::nullopt;
    }
    return query_result[0];
}

// Checks if the time is available - doesn't block.
bool TimestampPool::Handle::has_start() const {
    return this->has_time_impl(0).has_value();
}

bool TimestampPool::Handle::has_end() const {
    return this->has_time_impl(1).has_value();
}

TimestampPool::~TimestampPool() {}

} // namespace low_latency