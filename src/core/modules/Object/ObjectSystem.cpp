#include "ObjectSystem.h"
#include "Motion.h"
#include "Collision/Collision.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

namespace core::modules::Object {
namespace {
struct Dispatch {
    bool& active;
    explicit Dispatch(bool& value):active(value) {
        if(active) throw std::logic_error("Object batch dispatch cannot be reentered");
        active=true;
    }
    ~Dispatch() { active=false; }
};
}
ObjectSystem::ObjectSystem(std::uint32_t capacity):pool_(capacity),deletions_(capacity) {
    for(auto* list:{&order_,&activeOrder_,&snapshot_,&activate_,&sources_,&targets_}) list->reserve(capacity);
}
void ObjectSystem::Compact() noexcept {
    if(!orderDirty_) return;
    std::erase_if(order_,[&](auto h){ return !pool_.contains(h); });
    std::erase_if(activeOrder_,[&](auto h){ return !pool_.contains(h); });
    orderDirty_=false;
}
ObjectHandle ObjectSystem::Create() {
    if(finalized_) throw std::logic_error("Object service has shut down");
    Compact();
    const auto handle=pool_.create();
    if(handle.is_valid()) { order_.push_back(handle); ++validCount_; }
    return handle;
}
Object* ObjectSystem::Get(ObjectHandle h) noexcept { return pool_.get(h); }
const Object* ObjectSystem::Get(ObjectHandle h) const noexcept { return pool_.get(h); }
bool ObjectSystem::IsValid(ObjectHandle h) const noexcept {
    const auto* obj=Get(h); return obj && obj->lifecycle.status!=Object::Status::pendingDelete;
}
bool ObjectSystem::Active(ObjectHandle h) const noexcept {
    const auto* obj=Get(h); return obj && obj->lifecycle.status==Object::Status::active;
}
std::size_t ObjectSystem::GetObjectCount() const noexcept {
    return validCount_;
}
std::uint32_t ObjectSystem::GetPoolCapacity() const noexcept { return pool_.capacity(); }
std::vector<ObjectHandle> ObjectSystem::Snapshot(std::optional<std::int32_t> group) const {
    if(finalized_) throw std::logic_error("Object service has shut down");
    if(group && *group<0) throw std::invalid_argument("Object group must be non-negative");
    std::vector<ObjectHandle> result;
    result.reserve(validCount_);
    for(auto h:order_) {
        const auto* obj=Get(h);
        if(obj && IsValid(h) && (!group || obj->collision.group==group)) result.push_back(h);
    }
    return result;
}
bool ObjectSystem::IsColliding(ObjectHandle first,ObjectHandle second) const {
    if(!IsValid(first) || !IsValid(second)) throw std::invalid_argument("Object.IsColliding requires two valid objects");
    return first!=second && Intersects(first,second);
}
bool ObjectSystem::Delete(ObjectHandle h) noexcept {
    auto* obj=Get(h);
    if(!obj || obj->lifecycle.status==Object::Status::pendingDelete) return false;
    // At most pool capacity allocated objects can have one request each. The
    // queue never allocates, including while a deletion callback adds requests.
    deletions_[(deleteHead_+deleteCount_)%deletions_.size()]=h;
    ++deleteCount_;
    obj->lifecycle.status=Object::Status::pendingDelete; --validCount_; return true;
}
void ObjectSystem::Reclaim(ObjectHandle h,ObjectCallbacks& cb) noexcept {
    const bool valid=IsValid(h);
    if(!pool_.destroy(h)) return;
    if(valid) --validCount_;
    orderDirty_=true; cb.Release(h);
}
ObjectHandle ObjectSystem::PopDeletion() noexcept {
    const auto h=deletions_[deleteHead_];
    deleteHead_=(deleteHead_+1)%deletions_.size(); --deleteCount_;
    return h;
}
void ObjectSystem::Commit(ObjectCallbacks& cb) {
    if(closing_) throw std::logic_error("Object.Commit is unavailable during shutdown");
    Dispatch dispatch(dispatching_); Compact(); activate_.clear();
    // Freeze BOTH queues before the first callback. Only these requests belong
    // to this boundary; callback additions remain queued for another Commit.
    const auto pendingDeletes=deleteCount_;
    for(auto h:order_) {
        if(Get(h)->lifecycle.status==Object::Status::pendingActivation) activate_.push_back(h);
    }
    for(std::size_t i=0;i<pendingDeletes;++i) {
        const auto h=PopDeletion();
        try { cb.Delete(h); }
        catch(...) { Reclaim(h,cb); throw; }
        Reclaim(h,cb);
    }
    Compact();
    for(auto h:activate_) {
        auto* obj=Get(h);
        if(obj && obj->lifecycle.status==Object::Status::pendingActivation) {
            activeOrder_.push_back(h);
            obj->lifecycle.status=Object::Status::active;
        }
    }
}
void ObjectSystem::BeginShutdown() noexcept { closing_=true; }
bool ObjectSystem::FinalizeShutdown(ObjectCallbacks& cb) {
    if(finalized_) return cleanupSucceeded_;
    Dispatch dispatch(dispatching_); BeginShutdown();
    // Unlike normal Commit this is a drain, not an entry snapshot. New delete
    // requests append in FIFO order, and a failed callback cannot stop cleanup.
    while(deleteCount_) {
        const auto h=PopDeletion();
        try { cb.Delete(h); }
        catch(const std::exception& error) { cleanupSucceeded_=false; cb.CleanupError(error.what()); }
        catch(...) { cleanupSucceeded_=false; cb.CleanupError("unknown Object deletion error during shutdown"); }
        Reclaim(h,cb);
    }
    // No synthetic Delete calls for survivors, no pending activation.
    for(auto h:order_) Reclaim(h,cb);
    order_.clear(); activeOrder_.clear(); snapshot_.clear(); activate_.clear(); sources_.clear(); targets_.clear();
    deleteHead_=deleteCount_=validCount_=0; orderDirty_=false; finalized_=true;
    return cleanupSucceeded_;
}
void ObjectSystem::Update(ObjectCallbacks& cb) {
    Dispatch dispatch(dispatching_); Compact(); snapshot_.assign(activeOrder_.begin(),activeOrder_.end());
    for(auto h:snapshot_) {
        if(!Active(h)) continue;
        auto* obj=Get(h);
        if(!obj->scheduling.update) continue;
        if(obj->lifecycle.timer==Object::maximum_timer) throw std::overflow_error("Object timer overflow");
        // Commit both motion axes and timer before Lua; no pool pointer survives
        // callbacks; all subsequent accesses resolve the generation again.
        Object candidate=*obj; IntegrateMotion(candidate); ++candidate.lifecycle.timer; *obj=candidate;
        cb.Update(h);
    }
}
void ObjectSystem::Render(ObjectCallbacks& cb) {
    Dispatch dispatch(dispatching_); Compact(); snapshot_.assign(activeOrder_.begin(),activeOrder_.end());
    for(auto h:snapshot_) if(Active(h) && Get(h)->scheduling.render) cb.Render(h);
}
bool ObjectSystem::Candidate(ObjectHandle h,std::int32_t group) const noexcept {
    const auto* obj=Get(h);
    return obj && obj->lifecycle.status==Object::Status::active && obj->collision.enabled && obj->collision.group==group;
}
bool ObjectSystem::Intersects(ObjectHandle first,ObjectHandle second) const {
    const auto collider=[](const Object& obj) {
        return Collision::Collider{obj.transform.x,obj.transform.y,obj.collision.a,obj.collision.b,obj.transform.rot,obj.collision.rectangle};
    };
    return Collision::Intersects(collider(*Get(first)),collider(*Get(second)));
}
void ObjectSystem::Collide(std::int32_t groupA,std::int32_t groupB,ObjectCallbacks& cb) {
    if(groupA<0 || groupB<0) throw std::invalid_argument("Object collision groups must be non-negative");
    Dispatch dispatch(dispatching_); Compact(); sources_.clear(); targets_.clear();
    for(auto h:activeOrder_) {
        if(!Active(h)) continue;
        const auto group=Get(h)->collision.group;
        if(group==groupA) sources_.push_back(h);
        if(groupA!=groupB && group==groupB) targets_.push_back(h);
    }
    if(groupA==groupB) {
        for(std::size_t i=0;i<sources_.size();++i) for(std::size_t j=i+1;j<sources_.size();++j) {
            const auto a=sources_[i],b=sources_[j];
            if(!Candidate(a,groupA)) break;
            if(!Candidate(b,groupA) || !Intersects(a,b)) continue;
            cb.Collision(a,b);
            // This pair's geometry is evaluated once. Live eligibility is checked
            // again; changed positions/sizes affect subsequent pairs.
            if(Candidate(a,groupA) && Candidate(b,groupA)) cb.Collision(b,a);
        }
    } else {
        for(auto a:sources_) for(auto b:targets_) {
            if(!Candidate(a,groupA)) break;
            if(Candidate(b,groupB) && Intersects(a,b)) cb.Collision(a,b);
        }
    }
}
void ObjectSystem::SetBound(double left,double right,double bottom,double top) {
    for(double value:{left,right,bottom,top})
        if(!std::isfinite(value)) throw std::invalid_argument("Object bounds must be finite");
    if(left>right || bottom>top) throw std::invalid_argument("Object bounds must be ordered");
    left_=left; right_=right; bottom_=bottom; top_=top; hasBound_=true;
}
ObjectSystem::Bound ObjectSystem::GetBound() const {
    if(!hasBound_) throw std::logic_error("Object.SetBound must be called before GetBound");
    return {left_,right_,bottom_,top_};
}
void ObjectSystem::CheckOutBound() {
    if(!hasBound_) throw std::logic_error("Object.SetBound must be called before CheckOutBound");
    Dispatch dispatch(dispatching_); Compact();
    for(auto h:activeOrder_) {
        const auto* obj=Get(h);
        if(Active(h) && obj->lifecycle.auto_bound && (obj->transform.x<left_ || obj->transform.x>right_
            || obj->transform.y<bottom_ || obj->transform.y>top_)) Delete(h);
    }
}
}
