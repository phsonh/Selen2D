#include "Object.h"
#include "core/modules/Object/ObjectSystem.h"
#include "core/runtime/Log/Log.h"
#include "lua_bind/Guard.h"
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lua_bind::modules::Object {
namespace {
namespace Native=core::modules::Object;
constexpr const char* InstanceType="Solongo.Object.Instance";
constexpr const char* ServiceType="Solongo.Object.Service";
constexpr const char* ClassType="Solongo.Object.Class";
constexpr const char* IteratorType="Object.Iterator";
char ClassKey,ContextKey;
struct StackRestore {
    lua_State* L; int top;
    explicit StackRestore(lua_State* state):L(state),top(lua_gettop(state)) {}
    ~StackRestore() { lua_settop(L,top); }
};
// Tokens identify the originating service, never own it or reconnect old handles.
struct ServiceToken { void* owner{}; };
struct ServiceBox { static constexpr std::uint64_t tag=0x534f424a53455256ULL; std::shared_ptr<ServiceToken> token; };
struct InstanceBox { static constexpr std::uint64_t tag=0x534f424a494e5354ULL; std::shared_ptr<ServiceToken> token; Native::ObjectHandle handle; };
struct IteratorBox {
    static constexpr std::uint64_t tag=0x4f424a4954455232ULL;
    std::shared_ptr<ServiceToken> token;
    std::vector<Native::ObjectHandle> handles;
    std::size_t cursor{};
    std::optional<std::int32_t> group;
};
template<class Box> struct LuaSlot { std::uint64_t tag{Box::tag}; Box* value{}; };
template<class Box> Box** NewSlot(lua_State* L) {
    auto* slot=new(lua_newuserdata(L,sizeof(LuaSlot<Box>))) LuaSlot<Box>{}; return &slot->value;
}
template<class Box> Box** TrySlot(lua_State* L,int index,const char* type) {
    if(lua_type(L,index)!=LUA_TUSERDATA || lua_objlen(L,index)!=sizeof(LuaSlot<Box>) || !lua_getmetatable(L,index)) return nullptr;
    luaL_getmetatable(L,type); const bool same=lua_rawequal(L,-1,-2)!=0; lua_pop(L,2);
    if(!same) return nullptr;
    auto* slot=static_cast<LuaSlot<Box>*>(lua_touserdata(L,index)); return slot->tag==Box::tag?&slot->value:nullptr;
}
void Count(lua_State* L,int lo,int hi) { if(lua_gettop(L)<lo || lua_gettop(L)>hi) throw std::invalid_argument("Object: invalid argument count"); }
double Number(lua_State* L,int i,std::string_view name) {
    if(lua_type(L,i)!=LUA_TNUMBER || !std::isfinite(lua_tonumber(L,i))) throw std::invalid_argument("Object "+std::string(name)+" must be finite number");
    return lua_tonumber(L,i);
}
double Nonnegative(lua_State* L,int i,std::string_view name) {
    const double v=Number(L,i,name); if(v<0) throw std::invalid_argument("Object "+std::string(name)+" must be non-negative"); return v;
}
std::int32_t Integer(lua_State* L,int i,std::string_view name,bool signedValue=false) {
    const double v=Number(L,i,name);
    if(v<(signedValue?double(std::numeric_limits<std::int32_t>::min()):0.) || v>std::numeric_limits<std::int32_t>::max() || std::floor(v)!=v)
        throw std::invalid_argument("Object "+std::string(name)+" exceeds its integer range");
    return static_cast<std::int32_t>(v);
}
bool Boolean(lua_State* L,int i,std::string_view name) {
    if(lua_type(L,i)!=LUA_TBOOLEAN) throw std::invalid_argument("Object "+std::string(name)+" must be boolean");
    return lua_toboolean(L,i)!=0;
}
bool Hook(std::string_view key) { return key=="Init" || key=="Update" || key=="Render" || key=="Colli" || key=="Delete"; }
bool NativeField(std::string_view key) {
    return key=="x" || key=="y" || key=="rot" || key=="vx" || key=="vy" || key=="ax" || key=="ay" || key=="rate"
        || key=="a" || key=="b" || key=="group" || key=="rect" || key=="colli" || key=="bound" || key=="update" || key=="render" || key=="timer";
}
void ValidKey(lua_State* L,int i) {
    if(lua_isnil(L,i) || (lua_type(L,i)==LUA_TNUMBER && std::isnan(lua_tonumber(L,i)))) throw std::invalid_argument("Object custom key cannot be nil or NaN");
}
void PushClass(lua_State* L,int instance) {
    lua_getfenv(L,instance); lua_pushlightuserdata(L,&ClassKey); lua_rawget(L,-2); lua_remove(L,-2);
    if(lua_type(L,-1)!=LUA_TTABLE) throw std::runtime_error("Object class metadata unavailable");
}
std::string CallbackError(lua_State* L,const char* hook,Native::ObjectHandle h) {
    std::string message="Object class."+std::string(hook)+" [slot "+std::to_string(h.index)+"]: ";
    if(lua_type(L,-1)==LUA_TSTRING) { std::size_t size{}; const auto* text=lua_tolstring(L,-1,&size); message.append(text,size); }
    else message+="non-string Lua callback error";
    return message;
}
}
struct Context::Impl final:Native::ObjectCallbacks {
    struct Root { std::uint64_t generation{}; int reference{LUA_NOREF}; };
    lua_State* L; lua_State* callbackState;
    std::shared_ptr<ServiceToken> token{std::make_shared<ServiceToken>()};
    std::unique_ptr<Native::ObjectSystem> system;
    std::vector<Root> roots;
    int classes{LUA_NOREF}; // Weak class keys -> strong direct parent, or true for root.
    Native::ObjectHandle readableDeleted;
    std::uint32_t callbackDepth{},nativeEntryDepth{};
    bool closing{},shutdownRequested{},finalized{},cleanupFailed{};
    std::string cleanupError;
    Impl(lua_State* state,std::uint32_t capacity):L(state),callbackState(state),system(std::make_unique<Native::ObjectSystem>(capacity)),roots(capacity) {
        if(!state) throw std::invalid_argument("Object Context requires a Lua VM"); token->owner=this;
    }
    struct Entry {
        Impl& owner; lua_State* state; lua_State* previous;
        Entry(Impl& o,lua_State* s):owner(o),state(s),previous(o.callbackState) { ++owner.nativeEntryDepth; owner.callbackState=s; }
        ~Entry() { owner.callbackState=previous; if(--owner.nativeEntryDepth==0 && owner.shutdownRequested) owner.Finalize(state); }
    };
    struct Access {
        Impl& owner; Native::ObjectHandle previous;
        Access(Impl& o,Native::ObjectHandle h={}):owner(o),previous(o.readableDeleted) { owner.readableDeleted=h; }
        ~Access() { owner.readableDeleted=previous; }
    };
    struct Callback {
        Impl& owner;
        explicit Callback(Impl& o):owner(o) {
            if(owner.callbackDepth==std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("Object callback nesting overflow");
            ++owner.callbackDepth;
        }
        ~Callback() { --owner.callbackDepth; }
    };
    void RequireRunning() const { if(shutdownRequested) throw std::runtime_error("Object service has shut down"); }
    void RequireBatch() const { if(callbackDepth) throw std::logic_error("Object batch dispatch cannot run inside an Object callback"); }
    void BeginShutdown() noexcept {
        closing=true;
        if(system) system->BeginShutdown();
    }
    void CleanupError(const char* message) noexcept override {
        cleanupFailed=true;
        // Diagnostics must never prevent another queued object's reclamation.
        try { if(cleanupError.empty()) cleanupError=message; } catch(...) {}
        try { core::runtime::Log::Error(std::string("Object shutdown deletion failed: ")+message); } catch(...) {}
    }
    void Finalize(lua_State* state) noexcept {
        if(finalized) return;
        for(auto& root:roots) { if(root.reference!=LUA_NOREF) luaL_unref(state,LUA_REGISTRYINDEX,root.reference); root={}; }
        if(classes!=LUA_NOREF) luaL_unref(state,LUA_REGISTRYINDEX,classes);
        classes=LUA_NOREF; system.reset();
        lua_pushlightuserdata(state,&ContextKey); lua_rawget(state,LUA_REGISTRYINDEX);
        const bool owned=lua_touserdata(state,-1)==this; lua_pop(state,1);
        if(owned) { lua_pushlightuserdata(state,&ContextKey); lua_pushnil(state); lua_rawset(state,LUA_REGISTRYINDEX); }
        finalized=true;
    }
    void Shutdown() noexcept {
        if(shutdownRequested) return; shutdownRequested=true; token->owner=nullptr;
        if(!nativeEntryDepth) Finalize(L);
    }
    static bool OwnsVM(lua_State* state,const Impl& owner) {
        lua_pushlightuserdata(state,&ContextKey); lua_rawget(state,LUA_REGISTRYINDEX);
        const bool same=lua_type(state,-1)==LUA_TLIGHTUSERDATA && lua_touserdata(state,-1)==&owner; lua_pop(state,1); return same;
    }
    static Impl& Service(lua_State* state) {
        auto** slot=TrySlot<ServiceBox>(state,lua_upvalueindex(1),ServiceType);
        if(!slot || !*slot || !(*slot)->token || !(*slot)->token->owner) throw std::runtime_error("Object service has shut down");
        auto& owner=*static_cast<Impl*>((*slot)->token->owner);
        if(!owner.system || !OwnsVM(state,owner)) throw std::runtime_error("Object service unavailable in this VM"); return owner;
    }
    static InstanceBox& Box(lua_State* state,int index) {
        auto** slot=TrySlot<InstanceBox>(state,index,InstanceType);
        if(!slot || !*slot) throw std::invalid_argument("expected an Object instance"); return **slot;
    }
    static Impl& Owner(InstanceBox& box) {
        if(!box.token || !box.token->owner) throw std::runtime_error("Object instance service has shut down");
        return *static_cast<Impl*>(box.token->owner);
    }
    static Native::Object& Self(lua_State* state,InstanceBox& box,bool write=false) {
        auto& owner=Owner(box);
        if(!OwnsVM(state,owner)) throw std::invalid_argument("Object instance belongs to another VM");
        auto* object=owner.system->Get(box.handle);
        if(!object || (!owner.system->IsValid(box.handle) && (write || box.handle!=owner.readableDeleted)))
            throw std::runtime_error("Object instance is pending deletion or expired");
        return *object;
    }
    bool HasRoot(Native::ObjectHandle h) const noexcept {
        return h.index<roots.size() && roots[h.index].generation==h.generation && roots[h.index].reference!=LUA_NOREF;
    }
    void PushInstance(lua_State* state,Native::ObjectHandle h) {
        if(!HasRoot(h)) throw std::runtime_error("Object callback has no rooted instance");
        lua_rawgeti(state,LUA_REGISTRYINDEX,roots[h.index].reference);
    }
    void Parent(lua_State* state,int index) {
        lua_rawgeti(state,LUA_REGISTRYINDEX,classes); lua_pushvalue(state,index); lua_rawget(state,-2); lua_remove(state,-2);
    }
    void ValidateClass(lua_State* state,int index) {
        if(lua_type(state,index)!=LUA_TTABLE) throw std::invalid_argument("Object.New/Class requires a registered Object class");
        Parent(state,index); const bool registered=!lua_isnil(state,-1); lua_pop(state,1);
        if(!registered) throw std::invalid_argument("Object class was not registered by this service");
    }
    void Lookup(lua_State* state,int classIndex,int keyIndex) {
        // Constant stack depth; no Lua metamethod execution during inheritance.
        lua_pushvalue(state,classIndex);
        const int current=lua_gettop(state);
        while(lua_istable(state,current)) {
            lua_pushvalue(state,keyIndex); lua_rawget(state,current);
            if(!lua_isnil(state,-1)) { lua_remove(state,current); return; }
            lua_pop(state,1); Parent(state,current); lua_replace(state,current);
        }
        lua_pop(state,1); lua_pushnil(state);
    }
    void LookupHook(lua_State* state,int classIndex,const char* hook) {
        ValidateClass(state,classIndex);
        lua_pushstring(state,hook); const int key=lua_gettop(state); Lookup(state,classIndex,key); lua_remove(state,key);
        if(!lua_isnil(state,-1) && !lua_isfunction(state,-1)) throw std::invalid_argument("Object class."+std::string(hook)+" must be function or nil");
    }
    void Invoke(Native::ObjectHandle h,const char* hook,Native::ObjectHandle other={}) {
        RequireRunning(); auto* state=callbackState; StackRestore stack(state);
        Access access(*this,std::string_view(hook)=="Delete"?h:Native::ObjectHandle{});
        Callback callback(*this);
        if(!lua_checkstack(state,10)) throw std::runtime_error("Object callback stack exceeded");
        PushInstance(state,h); const int instance=lua_gettop(state);
        PushClass(state,instance); LookupHook(state,lua_gettop(state),hook);
        if(lua_isnil(state,-1)) return;
        lua_pushvalue(state,instance); if(other.is_valid()) PushInstance(state,other);
        const int result=lua_pcall(state,other.is_valid()?2:1,0,0); RequireRunning();
        if(result) throw std::runtime_error(CallbackError(state,hook,h));
    }
    void Update(Native::ObjectHandle h) override { Invoke(h,"Update"); }
    void Render(Native::ObjectHandle h) override { Invoke(h,"Render"); }
    void Collision(Native::ObjectHandle a,Native::ObjectHandle b) override { Invoke(a,"Colli",b); }
    void Delete(Native::ObjectHandle h) override { Invoke(h,"Delete"); }
    void Release(Native::ObjectHandle h) noexcept override {
        if(!HasRoot(h)) return; auto& root=roots[h.index]; luaL_unref(callbackState,LUA_REGISTRYINDEX,root.reference); root={};
    }
    static int CollectInstance(lua_State* state) {
        auto** slot=TrySlot<InstanceBox>(state,1,InstanceType); if(!slot || !*slot) return 0;
        auto& box=**slot;
        if(box.token && box.token->owner && static_cast<Impl*>(box.token->owner)->HasRoot(box.handle)) return 0;
        delete *slot; *slot=nullptr; return 0;
    }
    static int CollectService(lua_State* state) {
        auto** slot=TrySlot<ServiceBox>(state,1,ServiceType); if(slot) { delete *slot; *slot=nullptr; } return 0;
    }
    static int CollectIterator(lua_State* state) {
        auto** slot=TrySlot<IteratorBox>(state,1,IteratorType); if(slot) { delete *slot; *slot=nullptr; } return 0;
    }
    static int IteratorNext(lua_State* state) {
        // Generic for supplies its state/control values. They are deliberately
        // unused: the single returned closure owns its snapshot and cursor.
        auto** slot=TrySlot<IteratorBox>(state,lua_upvalueindex(1),IteratorType);
        if(!slot || !*slot) throw std::runtime_error("Object iterator is unavailable");
        auto& iterator=**slot;
        if(iterator.cursor==iterator.handles.size()) { lua_pushnil(state); return 1; }
        if(!iterator.token || !iterator.token->owner) throw std::runtime_error("Object iterator service has shut down");
        auto& owner=*static_cast<Impl*>(iterator.token->owner); Entry entry(owner,state);
        if(!OwnsVM(state,owner)) throw std::invalid_argument("Object iterator belongs to another VM");
        owner.RequireRunning();
        while(iterator.cursor<iterator.handles.size()) {
            const auto h=iterator.handles[iterator.cursor++];
            if(!owner.system->IsValid(h)) continue;
            if(iterator.group && owner.system->Get(h)->collision.group!=iterator.group) continue;
            owner.PushInstance(state,h); return 1;
        }
        lua_pushnil(state); return 1;
    }
    static int MakeIterator(lua_State* state,std::optional<std::int32_t> group) {
        auto& owner=Service(state); Entry entry(owner,state);
        auto** slot=NewSlot<IteratorBox>(state);
        luaL_getmetatable(state,IteratorType); lua_setmetatable(state,-2);
        *slot=new IteratorBox{owner.token,owner.system->Snapshot(group),0,group};
        // The userdata is rooted by this closure, but only identities (not the
        // instances' registry references or native slots) are retained in it.
        lua_pushcclosure(state,Guard<IteratorNext>,1); return 1;
    }
    static int GetAll(lua_State* state) { Count(state,0,0); return MakeIterator(state,{}); }
    static int GetGroup(lua_State* state) { Count(state,1,1); return MakeIterator(state,Integer(state,1,"group")); }
    static int Class(lua_State* state) {
        Count(state,0,1); auto& owner=Service(state); Entry entry(owner,state);
        const bool parent=lua_gettop(state)==1 && !lua_isnil(state,1);
        if(parent) owner.ValidateClass(state,1);
        lua_newtable(state); const int result=lua_gettop(state);
        luaL_getmetatable(state,ClassType); lua_setmetatable(state,result);
        lua_rawgeti(state,LUA_REGISTRYINDEX,owner.classes); lua_pushvalue(state,result);
        if(parent) lua_pushvalue(state,1); else lua_pushboolean(state,true);
        lua_rawset(state,-3); lua_pop(state,1); owner.RequireRunning(); return 1;
    }
    static int ClassIndex(lua_State* state) {
        auto& owner=Service(state); Entry entry(owner,state); owner.ValidateClass(state,1);
        if(lua_type(state,2)==LUA_TSTRING && CheckString(state,2)=="base") {
            owner.Parent(state,1); if(!lua_istable(state,-1)) { lua_pop(state,1); lua_pushnil(state); }
        } else owner.Lookup(state,1,2);
        return 1;
    }
    static int ClassNewIndex(lua_State* state) {
        auto& owner=Service(state); Entry entry(owner,state); owner.ValidateClass(state,1); ValidKey(state,2);
        const auto key=lua_type(state,2)==LUA_TSTRING?CheckString(state,2):std::string_view{};
        if(key=="base") throw std::invalid_argument("Object class.base is read-only");
        if(Hook(key) && !lua_isnil(state,3) && !lua_isfunction(state,3)) throw std::invalid_argument("Object class callback must be function or nil");
        lua_pushvalue(state,2); lua_pushvalue(state,3); lua_rawset(state,1); return 0;
    }
    static int Index(lua_State* state) {
        Count(state,2,2); auto& box=Box(state,1); auto& owner=Owner(box); Entry entry(owner,state);
        const auto& o=Self(state,box);
        const auto key=lua_type(state,2)==LUA_TSTRING?CheckString(state,2):std::string_view{};
        if(key=="x") lua_pushnumber(state,o.transform.x);
        else if(key=="y") lua_pushnumber(state,o.transform.y);
        else if(key=="rot") lua_pushnumber(state,o.transform.rot);
        else if(key=="vx") lua_pushnumber(state,o.motion.vx);
        else if(key=="vy") lua_pushnumber(state,o.motion.vy);
        else if(key=="ax") lua_pushnumber(state,o.motion.ax);
        else if(key=="ay") lua_pushnumber(state,o.motion.ay);
        else if(key=="rate") lua_pushnumber(state,o.motion.rate);
        else if(key=="a") lua_pushnumber(state,o.collision.a);
        else if(key=="b") lua_pushnumber(state,o.collision.b);
        else if(key=="group") { if(o.collision.group) lua_pushinteger(state,*o.collision.group); else lua_pushnil(state); }
        else if(key=="rect") lua_pushboolean(state,o.collision.rectangle);
        else if(key=="colli") lua_pushboolean(state,o.collision.enabled);
        else if(key=="bound") lua_pushboolean(state,o.lifecycle.auto_bound);
        else if(key=="update") lua_pushboolean(state,o.scheduling.update);
        else if(key=="render") lua_pushboolean(state,o.scheduling.render);
        else if(key=="timer") lua_pushinteger(state,o.lifecycle.timer);
        else if(key=="class") PushClass(state,1);
        else {
            lua_getfenv(state,1); lua_pushvalue(state,2); lua_rawget(state,-2);
            if(!lua_isnil(state,-1)) return 1;
            lua_pop(state,2); PushClass(state,1); owner.Lookup(state,lua_gettop(state),2);
        }
        owner.RequireRunning(); return 1;
    }
    static int NewIndex(lua_State* state) {
        Count(state,3,3); auto& box=Box(state,1); auto& owner=Owner(box); Entry entry(owner,state);
        const auto key=lua_type(state,2)==LUA_TSTRING?CheckString(state,2):std::string_view{};
        auto& o=Self(state,box,NativeField(key));
        if(key=="class") throw std::invalid_argument("Object.class is read-only");
        if(key=="x") o.transform.x=Number(state,3,key);
        else if(key=="y") o.transform.y=Number(state,3,key);
        else if(key=="rot") o.transform.rot=Number(state,3,key);
        else if(key=="vx") o.motion.vx=Number(state,3,key);
        else if(key=="vy") o.motion.vy=Number(state,3,key);
        else if(key=="ax") o.motion.ax=Number(state,3,key);
        else if(key=="ay") o.motion.ay=Number(state,3,key);
        else if(key=="rate") o.motion.rate=Nonnegative(state,3,key);
        else if(key=="a") o.collision.a=Nonnegative(state,3,key);
        else if(key=="b") o.collision.b=Nonnegative(state,3,key);
        else if(key=="group") { if(lua_isnil(state,3)) o.collision.group.reset(); else o.collision.group=Integer(state,3,key); }
        else if(key=="rect") o.collision.rectangle=Boolean(state,3,key);
        else if(key=="colli") o.collision.enabled=Boolean(state,3,key);
        else if(key=="bound") o.lifecycle.auto_bound=Boolean(state,3,key);
        else if(key=="update") o.scheduling.update=Boolean(state,3,key);
        else if(key=="render") o.scheduling.render=Boolean(state,3,key);
        else if(key=="timer") o.lifecycle.timer=Integer(state,3,key,true);
        else { ValidKey(state,2); lua_getfenv(state,1); lua_pushvalue(state,2); lua_pushvalue(state,3); lua_rawset(state,-3); }
        owner.RequireRunning(); return 0;
    }
    static int New(lua_State* state) {
        const int arguments=lua_gettop(state);
        if(arguments<1) throw std::invalid_argument("Object.New requires a registered class");
        auto& owner=Service(state); Entry entry(owner,state); owner.ValidateClass(state,1);
        if(!lua_checkstack(state,arguments+12)) throw std::runtime_error("Object.New argument stack exceeded");
        auto** slot=NewSlot<InstanceBox>(state); const int instance=lua_gettop(state);
        luaL_getmetatable(state,InstanceType); lua_setmetatable(state,instance);
        lua_newtable(state); lua_pushlightuserdata(state,&ClassKey); lua_pushvalue(state,1); lua_rawset(state,-3); lua_setfenv(state,instance);
        *slot=new InstanceBox{owner.token,{}};
        lua_pushvalue(state,instance); const int reference=luaL_ref(state,LUA_REGISTRYINDEX);
        Native::ObjectHandle handle;
        try { owner.RequireRunning(); handle=owner.system->Create(); if(!handle.is_valid()) throw std::runtime_error("Object.New: pool is full"); }
        catch(...) { luaL_unref(state,LUA_REGISTRYINDEX,reference); throw; }
        (*slot)->handle=handle; owner.roots[handle.index]={handle.generation,reference};
        {
            Callback callback(owner); Access access(owner);
            owner.LookupHook(state,1,"Init");
            if(lua_isnil(state,-1)) lua_pop(state,1);
            else {
                lua_pushvalue(state,instance);
                for(int i=2;i<=arguments;++i) lua_pushvalue(state,i);
                const int result=lua_pcall(state,arguments,0,0); owner.RequireRunning();
                if(result) throw std::runtime_error(CallbackError(state,"Init",handle));
            }
            owner.RequireRunning();
        }
        // Init errors deliberately leave this established, rooted object and
        // its queued state intact. Lua may have retained self or created others.
        lua_settop(state,instance); return 1;
    }
    static int IsValid(lua_State* state) {
        Count(state,1,1); auto& owner=Service(state); Entry entry(owner,state);
        auto** slot=TrySlot<InstanceBox>(state,1,InstanceType);
        lua_pushboolean(state,slot && *slot && (*slot)->token==owner.token && owner.system->IsValid((*slot)->handle)); return 1;
    }
    static int DeleteInstance(lua_State* state) {
        Count(state,1,1); auto& owner=Service(state); Entry entry(owner,state); const auto& box=Box(state,1);
        if(box.token!=owner.token) throw std::invalid_argument("Object instance belongs to another Context");
        lua_pushboolean(state,owner.system->Delete(box.handle)); return 1;
    }
    static int UpdateAll(lua_State* state) {
        Count(state,0,0); auto& owner=Service(state); Entry entry(owner,state); owner.RequireBatch();
        owner.system->Update(owner); owner.RequireRunning(); return 0;
    }
    static int Commit(lua_State* state) {
        Count(state,0,0); auto& owner=Service(state); Entry entry(owner,state); owner.RequireBatch();
        if(owner.closing) throw std::logic_error("Object.Commit is unavailable during shutdown");
        owner.system->Commit(owner); owner.RequireRunning(); return 0;
    }
    static int IsColliding(lua_State* state) {
        Count(state,2,2); auto& owner=Service(state); Entry entry(owner,state);
        const auto& first=Box(state,1); const auto& second=Box(state,2);
        if(first.token!=owner.token || second.token!=owner.token) throw std::invalid_argument("Object instance belongs to another Context");
        lua_pushboolean(state,owner.system->IsColliding(first.handle,second.handle)); return 1;
    }
    static int GetPoolCapacity(lua_State* state) {
        Count(state,0,0); auto& owner=Service(state); Entry entry(owner,state);
        lua_pushnumber(state,static_cast<double>(owner.system->GetPoolCapacity())); return 1;
    }
    static int RenderAll(lua_State* state) {
        Count(state,0,0); auto& owner=Service(state); Entry entry(owner,state); owner.RequireBatch();
        owner.system->Render(owner); owner.RequireRunning(); return 0;
    }
    static int CheckCollision(lua_State* state) {
        Count(state,2,2); auto& owner=Service(state); Entry entry(owner,state); owner.RequireBatch();
        owner.system->Collide(Integer(state,1,"groupA"),Integer(state,2,"groupB"),owner); owner.RequireRunning(); return 0;
    }
    static int GetObjectCount(lua_State* state) {
        Count(state,0,0); auto& owner=Service(state); Entry entry(owner,state);
        lua_pushnumber(state,static_cast<double>(owner.system->GetObjectCount())); return 1;
    }
    static int GetBound(lua_State* state) {
        Count(state,0,0); auto& owner=Service(state); Entry entry(owner,state);
        const auto b=owner.system->GetBound();
        lua_pushnumber(state,b.left); lua_pushnumber(state,b.right); lua_pushnumber(state,b.bottom); lua_pushnumber(state,b.top); return 4;
    }
    static int SetBound(lua_State* state) {
        Count(state,4,4); auto& owner=Service(state); Entry entry(owner,state);
        owner.system->SetBound(Number(state,1,"left"),Number(state,2,"right"),Number(state,3,"bottom"),Number(state,4,"top")); return 0;
    }
    static int CheckOutBound(lua_State* state) {
        Count(state,0,0); auto& owner=Service(state); Entry entry(owner,state); owner.RequireBatch(); owner.system->CheckOutBound(); return 0;
    }
};
Context::Context(lua_State* L,std::uint32_t capacity):impl_(std::make_unique<Impl>(L,capacity)) {}
Context::~Context() { Shutdown(); }
void Context::Shutdown() noexcept { if(impl_) impl_->Shutdown(); }
void Context::BeginShutdown() noexcept { if(impl_) impl_->BeginShutdown(); }
bool Context::FinalizeShutdown(std::string& error) {
    if(!impl_) { error.clear(); return true; }
    auto& owner=*impl_;
    if(!owner.finalized) {
        if(owner.nativeEntryDepth || owner.callbackDepth) throw std::logic_error("Object shutdown cannot finalize inside an Object call");
        owner.BeginShutdown();
        Impl::Entry entry(owner,owner.L);
        try { if(!owner.system->FinalizeShutdown(owner)) owner.cleanupFailed=true; }
        catch(...) { owner.Shutdown(); throw; }
        owner.Shutdown(); // Physical release waits until Entry has unwound.
    }
    error=owner.cleanupError;
    if(owner.cleanupFailed && error.empty()) error="Object deletion failed during shutdown";
    return !owner.cleanupFailed;
}
void Register(lua_State* L,Context& context) {
    if(!L || !context.impl_ || context.impl_->L!=L || !context.impl_->token->owner) throw std::invalid_argument("Object.Register requires live Context for VM");
    StackRestore stack(L); auto& owner=*context.impl_; Context::Impl::Entry entry(owner,L);
    lua_pushlightuserdata(L,&ContextKey); lua_rawget(L,LUA_REGISTRYINDEX); const auto* old=lua_touserdata(L,-1); lua_pop(L,1);
    if(old && old!=&owner) throw std::logic_error("only one live Object Context per VM");
    lua_pushlightuserdata(L,&ContextKey); lua_pushlightuserdata(L,&owner); lua_rawset(L,LUA_REGISTRYINDEX);
    if(owner.classes==LUA_NOREF) {
        lua_newtable(L); lua_newtable(L); lua_pushliteral(L,"k"); lua_setfield(L,-2,"__mode"); lua_setmetatable(L,-2);
        owner.classes=luaL_ref(L,LUA_REGISTRYINDEX);
    }
    luaL_newmetatable(L,InstanceType);
    lua_pushcfunction(L,Guard<Context::Impl::Index>); lua_setfield(L,-2,"__index");
    lua_pushcfunction(L,Guard<Context::Impl::NewIndex>); lua_setfield(L,-2,"__newindex");
    lua_pushcfunction(L,Guard<Context::Impl::CollectInstance>); lua_setfield(L,-2,"__gc");
    lua_pushliteral(L,"Object.Instance"); lua_setfield(L,-2,"__metatable"); lua_pop(L,1);
    luaL_newmetatable(L,ServiceType);
    lua_pushcfunction(L,Guard<Context::Impl::CollectService>); lua_setfield(L,-2,"__gc");
    lua_pushliteral(L,"Object.Service"); lua_setfield(L,-2,"__metatable"); lua_pop(L,1);
    luaL_newmetatable(L,IteratorType);
    lua_pushcfunction(L,Guard<Context::Impl::CollectIterator>); lua_setfield(L,-2,"__gc");
    lua_pushliteral(L,"Object.Iterator"); lua_setfield(L,-2,"__metatable"); lua_pop(L,1);
    auto** slot=NewSlot<ServiceBox>(L); const int service=lua_gettop(L);
    luaL_getmetatable(L,ServiceType); lua_setmetatable(L,service); *slot=new ServiceBox{owner.token};
    luaL_newmetatable(L,ClassType);
    lua_pushvalue(L,service); lua_pushcclosure(L,Guard<Context::Impl::ClassIndex>,1); lua_setfield(L,-2,"__index");
    lua_pushvalue(L,service); lua_pushcclosure(L,Guard<Context::Impl::ClassNewIndex>,1); lua_setfield(L,-2,"__newindex");
    lua_pushliteral(L,"Object.Class"); lua_setfield(L,-2,"__metatable"); lua_pop(L,1);
    const luaL_Reg functions[]={
        {"Class",Guard<Context::Impl::Class>},{"New",Guard<Context::Impl::New>},{"Delete",Guard<Context::Impl::DeleteInstance>},
        {"IsValid",Guard<Context::Impl::IsValid>},{"UpdateAll",Guard<Context::Impl::UpdateAll>},{"RenderAll",Guard<Context::Impl::RenderAll>},
        {"CheckCollision",Guard<Context::Impl::CheckCollision>},{"SetBound",Guard<Context::Impl::SetBound>},{"CheckOutBound",Guard<Context::Impl::CheckOutBound>},
        {"GetObjectCount",Guard<Context::Impl::GetObjectCount>},{"GetBound",Guard<Context::Impl::GetBound>},
        {"GetPoolCapacity",Guard<Context::Impl::GetPoolCapacity>},{"GetAll",Guard<Context::Impl::GetAll>},
        {"GetGroup",Guard<Context::Impl::GetGroup>},{"IsColliding",Guard<Context::Impl::IsColliding>},{"Commit",Guard<Context::Impl::Commit>},
        {nullptr,nullptr}};
    luaL_openlib(L,"Object",functions,1); owner.RequireRunning();
}
}
