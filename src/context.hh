#ifndef CONTEXT_HH_
#define CONTEXT_HH_

namespace low_latency {

// A context class doesn't do much by itself. We just use it to provide a
// virtual destructor so we can store a bunch of shared_ptrs in the same
// container and rely on RTTI in the layer context. It also deletes the copy and
// move constructors for derived classes implicitly, and that's pretty much it.
//
// We _could_ do something weird and complicated where we define virtual pure
// hashing and equality functions so we can store them in an unordered_set, but
// it's just unnecessary complexity and doesn't allow us to perform 'do you exist'
// lookups without creating an object.
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