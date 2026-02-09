#ifndef LAYER_CONTEXT_HH_
#define LAYER_CONTEXT_HH_

#include <mutex>
#include <variant>

#include "device_context.hh"
#include "instance_context.hh"
#include "queue_context.hh"

// The purpose of this file is to provide a definition for the highest level
// entry point struct of our vulkan state.
//
// All Context structs have deleted copy/move constructors. This is because we
// want to be extremely explicit with how/when we delete things, and this allows
// us to use destructors for cleanup without much worry about weird copies
// floating around. Most contexts will probably live inside std::unique_ptr's as
// a result so they can be used in standard containers.

namespace low_latency {

template <typename T>
concept DispatchableType =
    std::same_as<std::remove_cvref_t<T>, VkInstance> ||
    std::same_as<std::remove_cvref_t<T>, VkDevice> ||
    std::same_as<std::remove_cvref_t<T>, VkPhysicalDevice> ||
    std::same_as<std::remove_cvref_t<T>, VkQueue>;

struct LayerContext {
  public:
    using ContextVariant = std::variant<std::unique_ptr<DeviceContext>,
                                        std::unique_ptr<InstanceContext>>;

  public:
    std::mutex mutex;
    std::unordered_map<void*, ContextVariant> contexts;
    std::uint64_t current_frame = 0;

  public:
    LayerContext();
    LayerContext(const LayerContext&) = delete;
    LayerContext(LayerContext&&) = delete;
    LayerContext operator==(const LayerContext&) = delete;
    LayerContext operator==(LayerContext&&) = delete;
    ~LayerContext();

  public:
    template <DispatchableType T> static void* get_key(const T& dt) {
        return *reinterpret_cast<void**>(dt);
    }

    template <typename T, DispatchableType DispatchableType>
        requires(!std::same_as<T, QueueContext>)
    T& get_context(const DispatchableType& dt) {
        const auto key = get_key(dt);

        const auto it = this->contexts.find(key);
        assert(it != std::end(this->contexts));

        const auto ptr = std::get_if<std::unique_ptr<T>>(&it->second);
        assert(ptr && *ptr);

        return **ptr;
    }

    // QueueContext's are actually owned by a device so look there instead.
    template <typename T, DispatchableType DispatchableType>
        requires(std::same_as<T, QueueContext>)
    T& get_context(const DispatchableType& dt) {

        const auto& device_context = this->get_context<DeviceContext>(dt);
        const auto& queue_context = device_context.queue_contexts;

        const auto it = device_context.queue_contexts.find(dt);
        assert(it != std::end(queue_context));

        const auto& ptr = it->second;
        return *ptr;
    }
};

}; // namespace low_latency

#endif