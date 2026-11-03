#pragma once
#include "ObjectPool.h"
#include <vector>

namespace core::modules::Object {
struct ObjectCallbacks {
    virtual ~ObjectCallbacks()=default;
    virtual void Update(ObjectHandle)=0;
    virtual void Render(ObjectHandle)=0;
    virtual void Collision(ObjectHandle,ObjectHandle)=0;
    virtual void Delete(ObjectHandle)=0;
    virtual void Release(ObjectHandle) noexcept=0;
    // Shutdown diagnostics must not throw or enter Lua. Normal Commit errors
    // propagate to its caller instead of going through this cleanup channel.
    virtual void CleanupError(const char*) noexcept {}
};
class ObjectSystem final {
public:
    explicit ObjectSystem(std::uint32_t capacity);
    ObjectSystem(const ObjectSystem&)=delete;
    ObjectSystem& operator=(const ObjectSystem&)=delete;
    ObjectHandle Create();
    Object* Get(ObjectHandle) noexcept;
    const Object* Get(ObjectHandle) const noexcept;
    bool IsValid(ObjectHandle) const noexcept;
    std::size_t GetObjectCount() const noexcept;
    std::uint32_t GetPoolCapacity() const noexcept;
    std::vector<ObjectHandle> Snapshot(std::optional<std::int32_t> group={}) const;
    bool IsColliding(ObjectHandle,ObjectHandle) const;
    bool Delete(ObjectHandle) noexcept;
    void Commit(ObjectCallbacks&);
    void BeginShutdown() noexcept;
    bool FinalizeShutdown(ObjectCallbacks&);
    void Update(ObjectCallbacks&);
    void Render(ObjectCallbacks&);
    void Collide(std::int32_t groupA,std::int32_t groupB,ObjectCallbacks&);
    void SetBound(double left,double right,double bottom,double top);
    struct Bound { double left,right,bottom,top; };
    Bound GetBound() const;
    void CheckOutBound();
private:
    ObjectPool pool_;
    std::vector<ObjectHandle> order_,activeOrder_,snapshot_,activate_,deletions_,sources_,targets_;
    // A fixed-capacity FIFO contains each pending deletion exactly once. Pop
    // before invoking Lua so failure cannot retry it; callback additions append.
    std::size_t deleteHead_{},deleteCount_{},validCount_{};
    bool dispatching_{},orderDirty_{},hasBound_{},closing_{},finalized_{},cleanupSucceeded_{true};
    double left_{},right_{},bottom_{},top_{};
    bool Active(ObjectHandle) const noexcept;
    bool Candidate(ObjectHandle,std::int32_t) const noexcept;
    bool Intersects(ObjectHandle,ObjectHandle) const;
    void Compact() noexcept;
    void Reclaim(ObjectHandle,ObjectCallbacks&) noexcept;
    ObjectHandle PopDeletion() noexcept;
};
}
