#ifndef CONTEXT_HH_
#define CONTEXT_HH_

namespace low_latency {

// Context provides a pure virtual destructor so we can store dispatchable
// handles of Vulkan objects in a single map.
class Context {
  public:
    Context();
    Context(const Context& context) = delete;
    Context(Context&& context) = delete;
    Context operator=(const Context& context) = delete;
    Context operator=(Context&& context) = delete;
    virtual ~Context();
};

} // namespace low_latency

#endif