#ifndef CONTEXT_HH_
#define CONTEXT_HH_

// The purpose of this class is to provide a base class for Context classes.

namespace low_latency {

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