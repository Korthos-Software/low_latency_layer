#include "timestamp_pool.hh"
#include "device_context.hh"
#include "queue_context.hh"

#include <ranges>
#include <vulkan/vulkan_core.h>

namespace low_latency {

TimestampPool::Block TimestampPool::allocate() {
    const auto& device_context = this->queue_context.device_context;

    const auto query_pool = [&]() -> VkQueryPool {
        const auto qpci = VkQueryPoolCreateInfo{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = this->TIMESTAMP_QUERY_POOL_SIZE};

        auto query_pool = VkQueryPool{};

        device_context.vtable.CreateQueryPool(device_context.device, &qpci,
                                              nullptr, &query_pool);
        return query_pool;
    }();

    const auto key_range =
        std::views::iota(0u, this->TIMESTAMP_QUERY_POOL_SIZE / 2) |
        std::views::transform([](const std::uint64_t& i) { return 2 * i; });

    auto available_indices = std::make_unique<available_query_indicies_t>(
        available_query_indicies_t{std::begin(key_range), std::end(key_range)});

    auto command_buffers = [&, this]() -> auto {
        auto command_buffers =
            std::vector<VkCommandBuffer>(this->TIMESTAMP_QUERY_POOL_SIZE);

        const auto cbai = VkCommandBufferAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = this->queue_context.command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount =
                static_cast<std::uint32_t>(std::size(command_buffers)),
        };
        device_context.vtable.AllocateCommandBuffers(
            device_context.device, &cbai, std::data(command_buffers));
        std::ranges::for_each(command_buffers, [&](const auto& cb) {
            device_context.sdld(device_context.device, cb);
        });
        return std::make_unique<std::vector<VkCommandBuffer>>(command_buffers);
    }();

    return Block{.query_pool = query_pool,
                 .available_indicies = std::move(available_indices),
                 .command_buffers = std::move(command_buffers)};
}

TimestampPool::TimestampPool(QueueContext& queue_context)
    : queue_context(queue_context) {

    // Allocate one block on construction, it's likely more than enough!
    this->blocks.emplace_back(this->allocate());
}

std::unique_ptr<TimestampPool::Handle> TimestampPool::acquire() {
    const auto& vacant_iter = [this]() -> auto {
        const auto it =
            std::ranges::find_if(this->blocks, [](const auto& block) {
                return std::size(*block.available_indicies);
            });

        if (it != std::end(this->blocks)) {
            return it;
        }
        this->blocks.emplace_back(this->allocate());
        return std::prev(std::end(this->blocks));
    }();

    const auto query_pool = vacant_iter->query_pool;
    auto& available_indices = *vacant_iter->available_indicies;

    // Grab any element from our set and erase it immediately after.
    const auto query_index = *std::begin(available_indices);
    available_indices.erase(std::begin(available_indices));

    const auto command_buffers = [&]() -> auto {
        auto command_buffers = std::array<VkCommandBuffer, 2>{};
        std::ranges::copy_n(
            std::next(std::begin(*vacant_iter->command_buffers), query_index),
            std::size(command_buffers), std::begin(command_buffers));
        return command_buffers;
    }();

    const auto block_index = static_cast<std::size_t>(
        std::distance(std::begin(this->blocks), vacant_iter));

    return std::make_unique<Handle>(available_indices, block_index, query_pool,
                                    query_index, command_buffers);
}

TimestampPool::Handle::Handle(
    TimestampPool::available_query_indicies_t& index_origin,
    const std::size_t block_index, const VkQueryPool& query_pool,
    const std::uint64_t query_index,
    const std::array<VkCommandBuffer, 2>& command_buffers)
    : index_origin(index_origin), block_index(block_index),
      query_pool(query_pool), query_index(query_index),
      command_buffers(command_buffers) {}

TimestampPool::Handle::~Handle() {
    assert(this->index_origin.insert(this->query_index).second);
}

void TimestampPool::Handle::setup_command_buffers(
    const VkuDeviceDispatchTable& vtable) const {

    const auto& [head, tail] = this->command_buffers;

    const auto cbbi = VkCommandBufferBeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    // Heads
    vtable.ResetCommandBuffer(head, 0);
    vtable.BeginCommandBuffer(head, &cbbi);
    // Reset the next two and make them unavailable when they are run!
    vtable.CmdResetQueryPool(head, this->query_pool, this->query_index, 2);
    vtable.CmdWriteTimestamp2KHR(head, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                 this->query_pool, this->query_index);
    vtable.EndCommandBuffer(head);

    // Tails
    vtable.ResetCommandBuffer(tail, 0);
    vtable.BeginCommandBuffer(tail, &cbbi);
    vtable.CmdWriteTimestamp2KHR(tail, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                                 this->query_pool, this->query_index + 1);
    vtable.EndCommandBuffer(tail);
}

void TimestampPool::poll() {
    this->cached_timestamps.clear();
    this->cached_timestamps.reserve(std::size(this->blocks));

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

std::uint64_t TimestampPool::get_polled(const Handle& handle) {

    assert(handle.block_index < std::size(this->cached_timestamps));

    const auto& cached_timestamp = this->cached_timestamps[handle.block_index];
    assert(cached_timestamp != nullptr);
    assert(std::size(*cached_timestamp) < handle.query_index);

    return handle.query_index;
}

TimestampPool::~TimestampPool() {
    const auto& device = this->queue_context.device_context.device;
    const auto& vtable = this->queue_context.device_context.vtable;
    for (const auto& block : this->blocks) {
        vtable.FreeCommandBuffers(device, this->queue_context.command_pool,
                                  std::size(*block.command_buffers),
                                  std::data(*block.command_buffers));
        vtable.DestroyQueryPool(device, block.query_pool, nullptr);
    }
}

} // namespace low_latency