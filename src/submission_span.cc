#include "submission_span.hh"

namespace low_latency {

SubmissionSpan::SubmissionSpan(std::shared_ptr<TimestampPool::Handle> handle)
    : head_handle(std::move(handle)) {
    assert(this->head_handle); // Must not be null
}

SubmissionSpan::~SubmissionSpan() {}

void SubmissionSpan::update(std::shared_ptr<TimestampPool::Handle> handle) {
    this->tail_handle = std::move(handle);
}

DeviceClock::time_point SubmissionSpan::await_started() const {
    return this->head_handle->await_start();
}

std::pair<DeviceClock::time_point, DeviceClock::time_point>
SubmissionSpan::await_completed() const {
    if (this->tail_handle) {
        return {this->head_handle->await_start(),
                this->tail_handle->await_end()};
    }
    return {this->head_handle->await_start(), this->head_handle->await_end()};
}

bool SubmissionSpan::has_completed() const {
    if (this->tail_handle) {
        return this->tail_handle->has_end();
    }
    return this->head_handle->has_end();
}

} // namespace low_latency