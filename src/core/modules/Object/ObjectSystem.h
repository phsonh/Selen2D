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
    bool Delete(ObjectHandle) noexcept;
    void Discard(ObjectHandle,ObjectCallbacks&) noexcept; // Failed Init only; no game Delete.
    void BeginFrame(ObjectCallbacks&); // Host maintenance, never logic/render scheduling.
    void Update(ObjectCallbacks&);
    void Render(ObjectCallbacks&);
    void Collide(std::int32_t groupA,std::int32_t groupB,ObjectCallbacks&);
    void SetBound(double left,double right,double bottom,double top);
    struct Bound { double left,right,bottom,top; };
    Bound GetBound() const;
    void CheckOutBound();
private:
    ObjectPool pool_;
    std::vector<ObjectHandle> order_,snapshot_,activate_,remove_,sources_,targets_;
    bool dispatching_{},orderDirty_{},hasBound_{};
    double left_{},right_{},bottom_{},top_{};
    bool Active(ObjectHandle) const noexcept;
    bool Candidate(ObjectHandle,std::int32_t) const noexcept;
    bool Intersects(ObjectHandle,ObjectHandle) const;
    void Compact() noexcept;
};
}
