#include "Resource.h"
#include "core/modules/File/Vfs.h"
#include "core/modules/Resource/AudioAsset.h"
#include "core/modules/Resource/ImageProvider.h"
#include "lua_bind/Guard.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lua_bind::modules::Resource {
namespace {
namespace N=core::modules::Resource;
constexpr const char* ServiceType="engine.Resource.Service";
constexpr const char* LoaderType="engine.Resource.Loader";
char ContextKey;
struct Token {
    N::Service* service{};
    std::function<void(N::Job&)> wait;
    std::shared_ptr<const core::modules::File::FileSystem> files;
};
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
bool Options(lua_State* L,int index,std::initializer_list<std::string_view> fields) {
    if(lua_isnoneornil(L,index)) return false;
    if(lua_type(L,index)!=LUA_TTABLE) throw std::invalid_argument("Resource options must be a table or nil");
    // lua_next/rawget deliberately avoid __pairs, __index and __newindex.
    // Never coerce a numeric key in place while traversing the caller's table.
    lua_pushnil(L);
    while(lua_next(L,index)) {
        if(lua_type(L,-2)!=LUA_TSTRING) throw std::invalid_argument("Resource options keys must be strings");
        const auto key=CheckString(L,-2);
        if(std::find(fields.begin(),fields.end(),key)==fields.end())
            throw std::invalid_argument("unknown Resource option: "+std::string(key));
        lua_pop(L,1);
    }
    return true;
}
std::optional<double> OptionNumber(lua_State* L,int table,const char* key) {
    lua_pushstring(L,key); lua_rawget(L,table);
    std::optional<double> value;
    if(!lua_isnil(L,-1)) value=Number(L,-1);
    lua_pop(L,1);
    return value;
}
N::ImageRequest ImageOptions(lua_State* L,int index) {
    N::ImageRequest result;
    if(!Options(L,index,{"u","v","w","h","centerX","centerY","hscale","vscale","mipmap"})) return result;
    result.x=OptionNumber(L,index,"u").value_or(0);
    result.y=OptionNumber(L,index,"v").value_or(0);
    result.width=OptionNumber(L,index,"w"); result.height=OptionNumber(L,index,"h");
    result.centerX=OptionNumber(L,index,"centerX"); result.centerY=OptionNumber(L,index,"centerY");
    result.options.scale.x=OptionNumber(L,index,"hscale").value_or(1);
    result.options.scale.y=OptionNumber(L,index,"vscale").value_or(result.options.scale.x);
    lua_pushliteral(L,"mipmap"); lua_rawget(L,index);
    if(!lua_isnil(L,-1)) {
        if(lua_type(L,-1)!=LUA_TBOOLEAN) throw std::invalid_argument("mipmap must be a boolean");
        result.options.mipmap=lua_toboolean(L,-1)!=0;
    }
    lua_pop(L,1);
    return result;
}
N::AudioRequest AudioOptions(lua_State* L,int index,N::Kind kind) {
    N::AudioRequest result;
    if(kind==N::Kind::Music) {
        result.mode=N::AudioMode::Stream;
        result.loop=N::Loop{0,{}}; // A Music resource defaults to its entire source.
        if(Options(L,index,{"loopStart","loopEnd"})) {
            result.loop->start=OptionNumber(L,index,"loopStart").value_or(0);
            result.loop->finish=OptionNumber(L,index,"loopEnd");
        }
    }
    return result;
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
void Add(lua_State* L,N::Manifest& manifest,N::Kind kind,int offset,const Token& token) {
    const auto name=Name(L,offset+1);
    // Declaration-time checks cannot be postponed to a worker or Submit.
    // Actual I/O still happens asynchronously and rechecks the fixed VFS gate.
    const auto path=*token.files->NormalizePath(Text(L,offset+2,4096,"source path"));
    if(kind==N::Kind::Image) {
        manifest.LoadImage(std::string(path),std::string(name),ImageOptions(L,offset+3));
    } else if(kind==N::Kind::Sound || kind==N::Kind::Music) {
        manifest.LoadAudio(std::string(path),std::string(name),AudioOptions(L,offset+3,kind));
    } else if(kind==N::Kind::Font) manifest.LoadFont(std::string(path),std::string(name));
    else manifest.LoadShader(std::string(path),std::string(name));
}
template<N::Kind Kind,bool Async> int Load(lua_State* L) {
    constexpr int offset=Async?1:0;
    Count(L,offset+2,offset+((Kind==N::Kind::Image || Kind==N::Kind::Music)?3:2));
    if constexpr(Async) {
        auto& box=RequireBox<LoaderBox>(L,1,LoaderType); (void)Service(box.token);
        if(box.job) throw std::logic_error("Loader has already been submitted");
        Add(L,box.manifest,Kind,1,*box.token);
    } else {
        auto& token=Module(L).token;
        auto& service=Service(token);
        auto manifest=service.CreateManifest(); Add(L,manifest,Kind,0,*token);
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
int GetProgress(lua_State* L) {
    Count(L,1,1);
    auto& box=RequireBox<LoaderBox>(L,1,LoaderType); (void)Service(box.token);
    double progress{};
    if(box.job) {
        if(box.job->IsFinished()) { box.job->CheckResult(); progress=1; }
        else {
            const auto state=box.job->GetProgress();
            progress=static_cast<double>(state.prepared)/(static_cast<double>(state.requests)+1.0);
            progress=std::min(progress,std::nextafter(1.0,0.0));
        }
    }
    lua_pushnumber(L,progress); return 1;
}
int GetType(lua_State* L) {
    Count(L,1,1);
    const auto kind=Service(L).PublishedKind(std::string(Name(L,1)));
    if(!kind) { lua_pushnil(L); return 1; }
    switch(*kind) {
    case N::Kind::Image: lua_pushliteral(L,"Image"); break;
    case N::Kind::Sound: lua_pushliteral(L,"Sound"); break;
    case N::Kind::Music: lua_pushliteral(L,"Music"); break;
    case N::Kind::Font: lua_pushliteral(L,"Font"); break;
    case N::Kind::Shader: lua_pushliteral(L,"Shader"); break;
    }
    return 1;
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
    Impl(lua_State* state,N::Service& service,std::function<void(N::Job&)> wait,
        std::shared_ptr<const core::modules::File::FileSystem> files)
        :L(state),token(std::make_shared<Token>(Token{&service,std::move(wait),std::move(files)})) {
        if(!L || !token->wait || !token->files) throw std::invalid_argument("Resource Context requires Lua, VFS and a synchronous loading pump");
    }
};
std::shared_ptr<const N::Asset> AcquireImage(lua_State* L,int index,N::Service& service) {
    return Image(service,Find(service,Name(L,index)));
}
Context::Context(lua_State* L,N::Service& service,std::function<void(N::Job&)> wait,
    std::shared_ptr<const core::modules::File::FileSystem> files)
    :impl_(std::make_unique<Impl>(L,service,std::move(wait),std::move(files))) {}
Context::~Context() { Shutdown(); }
void Context::Shutdown() noexcept { if(impl_) { impl_->token->service=nullptr; impl_->token->wait={}; impl_->token->files.reset(); } }
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
        {"LoadShader",Guard<Load<N::Kind::Shader,true>>},{"Submit",Guard<Submit>},
        {"IsFinished",Guard<IsFinished>},{"GetProgress",Guard<GetProgress>},{nullptr,nullptr}};
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
        {"GetVolume",Guard<GetVolume>},{"SetVolume",Guard<SetVolume>},
        {"GetType",Guard<GetType>},{"Unload",Guard<Unload>},{nullptr,nullptr}};
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
