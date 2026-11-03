#include "Resource.h"
#include "core/modules/Resource/AudioAsset.h"
#include "core/modules/Resource/ImageProvider.h"
#include "lua_bind/Guard.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lua_bind::modules::Resource {
namespace {
namespace N=core::modules::Resource;
constexpr const char* ServiceType="Solongo.Resource.Service";
constexpr const char* LoaderType="Solongo.Resource.Loader";
char ContextKey;
struct Token { N::Service* service{}; std::function<void(N::Job&)> wait; };
struct ServiceBox {
    static constexpr std::uint64_t tag=0x5352455353455256ULL;
    std::shared_ptr<Token> token;
};
struct LoaderBox {
    static constexpr std::uint64_t tag=0x535245534c4f4144ULL;
    std::shared_ptr<Token> token;
    N::Manifest manifest;
    std::optional<N::Job> job;
};
template<class Box> struct Slot { std::uint64_t tag{Box::tag}; Box* value{}; };
template<class Box> Box** TrySlot(lua_State* L,int index,const char* type) {
    if(lua_type(L,index)!=LUA_TUSERDATA || lua_objlen(L,index)!=sizeof(Slot<Box>)) return nullptr;
    if(!lua_getmetatable(L,index)) return nullptr;
    luaL_getmetatable(L,type);
    const bool matches=lua_rawequal(L,-1,-2)!=0; lua_pop(L,2);
    if(!matches) return nullptr;
    auto* slot=static_cast<Slot<Box>*>(lua_touserdata(L,index));
    return slot->tag==Box::tag ? &slot->value : nullptr;
}
template<class Box> Box& RequireBox(lua_State* L,int index,const char* type) {
    auto** slot=TrySlot<Box>(L,index,type);
    if(!slot || !*slot) throw std::invalid_argument("invalid or released Resource handle");
    return **slot;
}
template<class Box> Box** NewSlot(lua_State* L,const char* type) {
    auto* slot=new(lua_newuserdata(L,sizeof(Slot<Box>))) Slot<Box>{};
    luaL_getmetatable(L,type); lua_setmetatable(L,-2);
    return &slot->value;
}
template<class Box> int Collect(lua_State* L,const char* type) {
    if(auto** slot=TrySlot<Box>(L,1,type)) { delete *slot; *slot=nullptr; }
    return 0;
}
int CollectService(lua_State* L) { return Collect<ServiceBox>(L,ServiceType); }
int CollectLoader(lua_State* L) { return Collect<LoaderBox>(L,LoaderType); }
void Count(lua_State* L,int minimum,int maximum) {
    const auto count=lua_gettop(L);
    if(count<minimum || count>maximum) throw std::invalid_argument("Resource: invalid argument count");
    if(!lua_checkstack(L,12)) throw std::runtime_error("Resource Lua stack capacity exceeded");
}
std::string_view Text(lua_State* L,int index,std::size_t limit,const char* label) {
    const auto value=CheckString(L,index);
    if(value.empty() || value.size()>limit || value.find('\0')!=std::string_view::npos)
        throw std::invalid_argument(std::string("invalid Resource ")+label);
    return value;
}
std::string_view Name(lua_State* L,int index) { return Text(L,index,512,"name"); }
double Number(lua_State* L,int index) {
    if(lua_type(L,index)!=LUA_TNUMBER || !std::isfinite(lua_tonumber(L,index)))
        throw std::invalid_argument("Resource expects a finite number");
    return lua_tonumber(L,index);
}
double Optional(lua_State* L,int index,double fallback) { return lua_isnoneornil(L,index)?fallback:Number(L,index); }
std::uint32_t Pixel(lua_State* L,int index,bool positive) {
    const auto number=Number(L,index);
    if(number<(positive?1.0:0.0) || number>std::numeric_limits<std::uint32_t>::max() || std::floor(number)!=number)
        throw std::invalid_argument("Image crop must use complete integer pixels and positive sizes");
    return static_cast<std::uint32_t>(number);
}
N::Service& Service(const std::shared_ptr<Token>& token) {
    if(!token || !token->service) throw std::runtime_error("Resource service has shut down");
    return *token->service;
}
ServiceBox& Module(lua_State* L) { return RequireBox<ServiceBox>(L,lua_upvalueindex(1),ServiceType); }
N::Service& Service(lua_State* L) { return Service(Module(L).token); }
N::Handle Find(N::Service& service,std::string_view name) {
    const auto handle=service.Find(std::string(name));
    if(!handle) throw std::invalid_argument("resource not found: "+std::string(name));
    return *handle;
}
std::shared_ptr<const N::Images::ImageAsset> Image(N::Service& service,N::Handle handle) {
    auto image=std::dynamic_pointer_cast<const N::Images::ImageAsset>(service.Acquire(handle));
    if(!image) throw std::invalid_argument("resource is not an Image");
    return image;
}
std::shared_ptr<const N::AudioAsset> AudioAsset(lua_State* L) {
    auto& service=Service(L);
    auto asset=std::dynamic_pointer_cast<const N::AudioAsset>(service.Acquire(Find(service,Name(L,1))));
    if(!asset) throw std::invalid_argument("resource is not Sound or Music");
    return asset;
}
void Add(lua_State* L,N::Manifest& manifest,N::Kind kind,int offset) {
    const auto name=Name(L,offset+1),path=Text(L,offset+2,4096,"source path");
    if(kind==N::Kind::Image) {
        int supplied{};
        for(int i=3;i<=6;++i) supplied+=!lua_isnoneornil(L,offset+i);
        if(supplied!=0 && supplied!=4) throw std::invalid_argument("Image crop requires all of u, v, w and h");
        N::ImageOptions options;
        const bool cx=!lua_isnoneornil(L,offset+7),cy=!lua_isnoneornil(L,offset+8);
        if(cx!=cy) throw std::invalid_argument("Image center requires both centerX and centerY");
        if(cx) options.anchor=N::Vec2{Number(L,offset+7),Number(L,offset+8)};
        const auto scale=Optional(L,offset+9,1); options.scale={scale,scale};
        if(!lua_isnoneornil(L,offset+10)) {
            if(lua_type(L,offset+10)!=LUA_TBOOLEAN) throw std::invalid_argument("mipmap must be a boolean");
            options.mipmap=lua_toboolean(L,offset+10)!=0;
        }
        if(supplied) manifest.LoadImage(std::string(path),std::string(name),Pixel(L,offset+3,false),Pixel(L,offset+4,false),
            Pixel(L,offset+5,true),Pixel(L,offset+6,true),options);
        else manifest.LoadImage(std::string(path),std::string(name),options);
    } else if(kind==N::Kind::Sound || kind==N::Kind::Music) {
        N::AudioRequest options;
        options.mode=kind==N::Kind::Sound?N::AudioMode::Decoded:N::AudioMode::Stream;
        if(kind==N::Kind::Music) {
            options.loop=N::Loop{Optional(L,offset+3,0),{}};
            if(!lua_isnoneornil(L,offset+4)) options.loop->finish=Number(L,offset+4);
        }
        manifest.LoadAudio(std::string(path),std::string(name),options);
    } else if(kind==N::Kind::Font) manifest.LoadFont(std::string(path),std::string(name));
    else manifest.LoadShader(std::string(path),std::string(name));
}
template<N::Kind Kind,bool Async> int Load(lua_State* L) {
    constexpr int offset=Async?1:0;
    Count(L,offset+2,offset+(Kind==N::Kind::Image?10:Kind==N::Kind::Music?4:2));
    if constexpr(Async) {
        auto& box=RequireBox<LoaderBox>(L,1,LoaderType); (void)Service(box.token);
        if(box.job) throw std::logic_error("Loader has already been submitted");
        Add(L,box.manifest,Kind,1);
    } else {
        auto& token=Module(L).token;
        auto& service=Service(token);
        auto manifest=service.CreateManifest(); Add(L,manifest,Kind,0);
        auto job=service.Submit(manifest);
        try { token->wait(job); job.CheckResult(); }
        catch(...) { job.Cancel(); throw; }
    }
    return 0;
}
int CreateLoader(lua_State* L) {
    Count(L,0,0);
    auto** slot=NewSlot<LoaderBox>(L,LoaderType);
    auto box=std::make_unique<LoaderBox>(); box->token=Module(L).token;
    box->manifest=Service(box->token).CreateManifest(); *slot=box.release(); return 1;
}
int Submit(lua_State* L) {
    Count(L,1,1);
    auto& box=RequireBox<LoaderBox>(L,1,LoaderType);
    if(box.job) throw std::logic_error("Loader has already been submitted");
    box.job=Service(box.token).Submit(box.manifest); return 0;
}
int IsFinished(lua_State* L) {
    Count(L,1,1);
    auto& box=RequireBox<LoaderBox>(L,1,LoaderType); (void)Service(box.token);
    const bool finished=box.job && box.job->IsFinished();
    if(finished) box.job->CheckResult();
    lua_pushboolean(L,finished); return 1;
}
enum class Property { Size,Center,Scale,Sampler };
template<Property P> int GetImage(lua_State* L) {
    Count(L,1,1);
    auto& service=Service(L);
    auto image=Image(service,Find(service,Name(L,1)));
    const auto& data=image->image;
    if constexpr(P==Property::Size) { lua_pushnumber(L,data.width); lua_pushnumber(L,data.height); }
    if constexpr(P==Property::Center) {
        if(!data.options.anchor) throw std::logic_error("Image center was not prepared");
        lua_pushnumber(L,data.options.anchor->x); lua_pushnumber(L,data.options.anchor->y);
    }
    if constexpr(P==Property::Scale) { lua_pushnumber(L,data.options.scale.x); lua_pushnumber(L,data.options.scale.y); }
    if constexpr(P==Property::Sampler) {
        const bool point=data.options.filter==N::Filter::Point,wrap=data.options.address==N::Address::Repeat;
        const char* result=point?(wrap?"point+wrap":"point+clamp"):(wrap?"linear+wrap":"linear+clamp");
        image.reset(); // No native snapshot destructor may be bypassed by Lua allocation.
        lua_pushstring(L,result); return 1;
    }
    return 2;
}
template<Property P> int SetImage(lua_State* L) {
    Count(L,P==Property::Center?3:2,P==Property::Sampler?2:3);
    auto& service=Service(L); const auto handle=Find(service,Name(L,1));
    auto next=std::make_shared<N::Images::ImageAsset>(*Image(service,handle));
    if constexpr(P==Property::Center) next->image.options.anchor=N::Vec2{Number(L,2),Number(L,3)};
    if constexpr(P==Property::Scale) { const auto x=Number(L,2); next->image.options.scale={x,Optional(L,3,x)}; }
    if constexpr(P==Property::Sampler) {
        const auto value=CheckString(L,2);
        if(value=="linear+clamp") { next->image.options.filter=N::Filter::Linear; next->image.options.address=N::Address::Clamp; }
        else if(value=="point+clamp") { next->image.options.filter=N::Filter::Point; next->image.options.address=N::Address::Clamp; }
        else if(value=="linear+wrap") { next->image.options.filter=N::Filter::Linear; next->image.options.address=N::Address::Repeat; }
        else if(value=="point+wrap") { next->image.options.filter=N::Filter::Point; next->image.options.address=N::Address::Repeat; }
        else throw std::invalid_argument("unknown Image sampler");
    }
    service.ReplaceAsset(handle,std::move(next)); return 0;
}
int SetVolume(lua_State* L) {
    Count(L,2,2); const auto volume=Number(L,2);
    if(volume<0 || volume>1) throw std::invalid_argument("resource volume must be in [0,1]");
    AudioAsset(L)->source->SetVolume(volume); return 0;
}
int GetVolume(lua_State* L) { Count(L,1,1); lua_pushnumber(L,AudioAsset(L)->source->GetVolume()); return 1; }
int Unload(lua_State* L) { Count(L,1,1); Service(L).Unload(std::string(Name(L,1))); return 0; }
void Metatable(lua_State* L,const char* type,lua_CFunction collect,const luaL_Reg* methods) {
    luaL_newmetatable(L,type);
    lua_pushcfunction(L,collect); lua_setfield(L,-2,"__gc");
    lua_pushstring(L,type); lua_setfield(L,-2,"__metatable");
    if(methods) { lua_newtable(L); luaL_register(L,nullptr,methods); lua_setfield(L,-2,"__index"); }
    lua_pop(L,1);
}
}
struct Context::Impl {
    lua_State* L; std::shared_ptr<Token> token;
    Impl(lua_State* state,N::Service& service,std::function<void(N::Job&)> wait)
        :L(state),token(std::make_shared<Token>(Token{&service,std::move(wait)})) {
        if(!L || !token->wait) throw std::invalid_argument("Resource Context requires Lua and a synchronous loading pump");
    }
};
std::shared_ptr<const N::Asset> AcquireImage(lua_State* L,int index,N::Service& service) {
    return Image(service,Find(service,Name(L,index)));
}
Context::Context(lua_State* L,N::Service& service,std::function<void(N::Job&)> wait)
    :impl_(std::make_unique<Impl>(L,service,std::move(wait))) {}
Context::~Context() { Shutdown(); }
void Context::Shutdown() noexcept { if(impl_) { impl_->token->service=nullptr; impl_->token->wait={}; } }
void Register(lua_State* L,Context& context) {
    if(!L || !context.impl_ || context.impl_->L!=L || !context.impl_->token->service)
        throw std::invalid_argument("Resource.Register requires a live Context for this Lua state");
    const int top=lua_gettop(L);
    struct Restore { lua_State* L; int top; ~Restore() { lua_settop(L,top); } } restore{L,top};
    lua_pushlightuserdata(L,&ContextKey); lua_rawget(L,LUA_REGISTRYINDEX);
    auto** previous=TrySlot<ServiceBox>(L,-1,ServiceType);
    if(previous && *previous && (*previous)->token->service && (*previous)->token!=context.impl_->token)
        throw std::logic_error("only one live Resource Context may register in a Lua state");
    lua_pop(L,1);
    const luaL_Reg methods[]={
        {"LoadImage",Guard<Load<N::Kind::Image,true>>},{"LoadSound",Guard<Load<N::Kind::Sound,true>>},
        {"LoadMusic",Guard<Load<N::Kind::Music,true>>},{"LoadFont",Guard<Load<N::Kind::Font,true>>},
        {"LoadShader",Guard<Load<N::Kind::Shader,true>>},{"Submit",Guard<Submit>},{"IsFinished",Guard<IsFinished>},{nullptr,nullptr}};
    Metatable(L,ServiceType,Guard<CollectService>,nullptr);
    Metatable(L,LoaderType,Guard<CollectLoader>,methods);
    auto** slot=NewSlot<ServiceBox>(L,ServiceType); *slot=new ServiceBox{context.impl_->token};
    lua_pushlightuserdata(L,&ContextKey); lua_pushvalue(L,-2); lua_rawset(L,LUA_REGISTRYINDEX);
    const int serviceSlot=lua_gettop(L);
    const luaL_Reg functions[]={
        {"CreateLoader",Guard<CreateLoader>},
        {"LoadImage",Guard<Load<N::Kind::Image,false>>},{"LoadSound",Guard<Load<N::Kind::Sound,false>>},
        {"LoadMusic",Guard<Load<N::Kind::Music,false>>},{"LoadFont",Guard<Load<N::Kind::Font,false>>},
        {"LoadShader",Guard<Load<N::Kind::Shader,false>>},
        {"GetImageSize",Guard<GetImage<Property::Size>>},
        {"GetImageCenter",Guard<GetImage<Property::Center>>},{"SetImageCenter",Guard<SetImage<Property::Center>>},
        {"GetImageScale",Guard<GetImage<Property::Scale>>},{"SetImageScale",Guard<SetImage<Property::Scale>>},
        {"GetImageSampler",Guard<GetImage<Property::Sampler>>},{"SetImageSampler",Guard<SetImage<Property::Sampler>>},
        {"GetVolume",Guard<GetVolume>},{"SetVolume",Guard<SetVolume>},{"Unload",Guard<Unload>},{nullptr,nullptr}};
    lua_newtable(L); const int module=lua_gettop(L);
    for(const auto* f=functions;f->name;++f) {
        lua_pushvalue(L,serviceSlot); lua_pushcclosure(L,f->func,1); lua_setfield(L,module,f->name);
    }
    lua_pushvalue(L,module); lua_setglobal(L,"Resource");
    lua_getfield(L,LUA_REGISTRYINDEX,"_LOADED");
    if(!lua_istable(L,-1)) throw std::logic_error("Lua loaded-module registry must be a table");
    lua_pushvalue(L,module); lua_setfield(L,-2,"Resource");
}
}
