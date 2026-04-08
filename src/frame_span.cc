#include "frame_span.hh"

namespace low_latency {

FrameSpan::FrameSpan(std::shared_ptr<TimestampPool::Handle> handle)
    : head_handle(std::move(handle)) {
    assert(this->head_handle); // Must not be null
}

FrameSpan::~FrameSpan() {}

void FrameSpan::update(std::shared_ptr<TimestampPool::Handle> handle) {
    this->tail_handle = std::move(handle);
}

void FrameSpan::await_completed() const {
    if (this->tail_handle) {
        this->tail_handle->await_end();
        return;
    }
    this->head_handle->await_end();
}

bool FrameSpan::has_completed() const {
    if (this->tail_handle) {
        return this->tail_handle->has_end();
    }
    return this->head_handle->has_end();
}

} // namespace low_latency