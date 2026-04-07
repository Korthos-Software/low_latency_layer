#ifndef HELPER_HH_
#define HELPER_HH_

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace low_latency {

#define THROW_NOT_VKSUCCESS(x)                                                 \
    if (const auto result = x; result != VK_SUCCESS) {                         \
        throw result;                                                          \
    }

// Small templates which allow us to SFINAE find pNext structs.
template <typename T>
static T* find_next(void* const head, const VkStructureType& stype) {
    for (auto i = reinterpret_cast<VkBaseOutStructure*>(head)->pNext; i;
         i = i->pNext) {

        if (i->sType == stype) {
            return reinterpret_cast<T*>(i);
        }
    }
    return nullptr;
}

template <typename T>
static const T* find_next(const void* const head,
                          const VkStructureType& stype) {

    for (auto i = reinterpret_cast<const VkBaseInStructure*>(head)->pNext; i;
         i = i->pNext) {

        if (i->sType == stype) {
            return reinterpret_cast<const T*>(i);
        }
    }
    return nullptr;
}

template <typename T>
static const T* find_link(const void* const head,
                          const VkStructureType& stype) {
    for (auto info = find_next<T>(head, stype); info;
         info = find_next<T>(info, stype)) {

        if (info->function == VK_LAYER_LINK_INFO) {
            return reinterpret_cast<const T*>(info);
        }
    }
    return nullptr;
}

} // namespace low_latency

#endif