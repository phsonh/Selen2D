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
ObjectSystem::ObjectSystem(std::uint32_t capacity):pool_(capacity) {
    for(auto* list:{&order_,&snapshot_,&activate_,&remove_,&sources_,&targets_}) list->reserve(capacity);
}
void ObjectSystem::Compact() noexcept {
    if(!orderDirty_) return;
    std::erase_if(order_,[&](auto h){ return !pool_.contains(h); }); orderDirty_=false;
}
ObjectHandle ObjectSystem::Create() {
    Compact();
    const auto handle=pool_.create();
    if(handle.is_valid()) order_.push_back(handle);
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
    return static_cast<std::size_t>(std::count_if(pool_.units().begin(),pool_.units().end(),[](const Object& obj) {
        return obj.lifecycle.status!=Object::Status::pendingDelete;
    }));
}
bool ObjectSystem::Delete(ObjectHandle h) noexcept {
    auto* obj=Get(h);
    if(!obj || obj->lifecycle.status==Object::Status::pendingDelete) return false;
    obj->lifecycle.status=Object::Status::pendingDelete; return true;
}
void ObjectSystem::Discard(ObjectHandle h,ObjectCallbacks& cb) noexcept {
    if(!pool_.destroy(h)) return;
    orderDirty_=true; cb.Release(h);
}
void ObjectSystem::BeginFrame(ObjectCallbacks& cb) {
    Dispatch dispatch(dispatching_); Compact(); activate_.clear(); remove_.clear();
    // Freeze BOTH queues before any Lua cleanup. New/Deleted in callbacks waits
    // for another real host boundary, even if Lua calls UpdateAll repeatedly.
    for(auto h:order_) {
        const auto status=Get(h)->lifecycle.status;
        if(status==Object::Status::pendingDelete) remove_.push_back(h);
        else if(status==Object::Status::pendingActivation) activate_.push_back(h);
    }
    std::exception_ptr failure;
    for(auto h:remove_) {
        try { cb.Delete(h); } catch(...) { if(!failure) failure=std::current_exception(); }
        Discard(h,cb);
    }
    for(auto h:activate_) {
        auto* obj=Get(h);
        if(obj && obj->lifecycle.status==Object::Status::pendingActivation) obj->lifecycle.status=Object::Status::active;
    }
    Compact();
    if(failure) std::rethrow_exception(failure);
}
void ObjectSystem::Update(ObjectCallbacks& cb) {
    Dispatch dispatch(dispatching_); Compact(); snapshot_.assign(order_.begin(),order_.end());
    for(auto h:snapshot_) {
        if(!Active(h)) continue;
        auto* obj=Get(h);
        if(!obj->scheduling.update) continue;
        if(obj->lifecycle.timer==Object::maximum_timer) throw std::overflow_error("Object timer overflow");
        // Commit both motion axes and timer before Lua; no pool pointer survives
        // callbacks (nested Init failure may move the dense array's last entry).
        Object candidate=*obj; IntegrateMotion(candidate); ++candidate.lifecycle.timer; *obj=candidate;
        cb.Update(h);
    }
}
void ObjectSystem::Render(ObjectCallbacks& cb) {
    Dispatch dispatch(dispatching_); Compact(); snapshot_.assign(order_.begin(),order_.end());
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
    for(auto h:order_) {
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
    for(auto h:order_) {
        const auto* obj=Get(h);
        if(Active(h) && obj->lifecycle.auto_bound && (obj->transform.x<left_ || obj->transform.x>right_
            || obj->transform.y<bottom_ || obj->transform.y>top_)) Delete(h);
    }
}
}
