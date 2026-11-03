#include "Graphic.h"
#include "Mesh.h"
#include "ShaderParams.h"
#include "lua_bind/Guard.h"
#include "lua_bind/modules/Resource/Resource.h"
#include "core/modules/Resource/ImageProvider.h"
#include "core/modules/Resource/ShaderProvider.h"
#include "core/modules/Graphic/render/Renderer.h"
#include "core/modules/Graphic/geometry/Ribbon.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace lua_bind::modules::Graphic {
namespace {
namespace R=core::modules::Graphic::Render;
namespace C=core::modules::Graphic::Canvas;
namespace A=core::modules::Resource;
namespace S=core::modules::Graphic::Geometry;
constexpr const char* ServiceType="Solongo.Graphic.Service";
constexpr const char* CanvasType="Solongo.Graphic.Canvas";
constexpr const char* CameraType="Solongo.Graphic.Camera";
constexpr const char* PngType="Solongo.Graphic.PngBuffer";
char ContextKey;
struct Token { R::Renderer* renderer{}; A::Service* resources{}; };
struct ServiceBox {
    static constexpr std::uint64_t tag=0x5347524150485356ULL;
    std::shared_ptr<Token> token;
};
struct CanvasBox {
    static constexpr std::uint64_t tag=0x5347524150484356ULL;
    std::shared_ptr<Token> token;
    C::Handle handle;
};
struct CameraBox {
    static constexpr std::uint64_t tag=0x5347524150484341ULL;
    std::shared_ptr<Token> token;
    C::Handle handle;
};
struct PngBox {
    static constexpr std::uint64_t tag=0x534752415048504eULL;
    std::string bytes;
};
template<class T> struct Slot { std::uint64_t tag{T::tag}; T* value{}; };
template<class T> T** TrySlot(lua_State* L,int index,const char* type) {
    if(lua_type(L,index)!=LUA_TUSERDATA || lua_objlen(L,index)!=sizeof(Slot<T>) || !lua_getmetatable(L,index)) return nullptr;
    luaL_getmetatable(L,type);
    const bool matches=lua_rawequal(L,-1,-2)!=0;
    lua_pop(L,2);
    if(!matches) return nullptr;
    auto* slot=static_cast<Slot<T>*>(lua_touserdata(L,index));
    return slot->tag==T::tag ? &slot->value : nullptr;
}
template<class T> T& Require(lua_State* L,int index,const char* type) {
    auto** slot=TrySlot<T>(L,index,type);
    if(!slot || !*slot) throw std::invalid_argument("invalid or released Graphic handle");
    return **slot;
}
template<class T> T** NewSlot(lua_State* L,const char* type) {
    auto* slot=new(lua_newuserdata(L,sizeof(Slot<T>))) Slot<T>{};
    luaL_getmetatable(L,type); lua_setmetatable(L,-2);
    return &slot->value;
}
template<class T> int Collect(lua_State* L,const char* type) {
    if(auto** slot=TrySlot<T>(L,1,type)) { delete *slot; *slot=nullptr; }
    return 0;
}
int CollectService(lua_State* L) { return Collect<ServiceBox>(L,ServiceType); }
int CollectCanvas(lua_State* L) { return Collect<CanvasBox>(L,CanvasType); }
int CollectCamera(lua_State* L) { return Collect<CameraBox>(L,CameraType); }
int CollectPng(lua_State* L) { return Collect<PngBox>(L,PngType); }
Token& Live(const std::shared_ptr<Token>& token) {
    if(!token || !token->renderer || !token->resources) throw std::runtime_error("Graphic Context has shut down");
    return *token;
}
ServiceBox& Module(lua_State* L) { return Require<ServiceBox>(L,lua_upvalueindex(1),ServiceType); }
Token& Host(lua_State* L) { return Live(Module(L).token); }
void Count(lua_State* L,int minimum,int maximum) {
    if(lua_gettop(L)<minimum || lua_gettop(L)>maximum) throw std::invalid_argument("Graphic: invalid argument count");
    if(!lua_checkstack(L,8)) throw std::runtime_error("Graphic Lua stack capacity exceeded");
}
double Number(lua_State* L,int index) {
    if(lua_type(L,index)!=LUA_TNUMBER || !std::isfinite(lua_tonumber(L,index)))
        throw std::invalid_argument("Graphic numeric arguments must be finite numbers");
    return lua_tonumber(L,index);
}
double Optional(lua_State* L,int index,double fallback) { return lua_isnoneornil(L,index) ? fallback:Number(L,index); }
std::uint32_t Integer(lua_State* L,int index,std::uint32_t maximum=std::numeric_limits<std::uint32_t>::max()) {
    const auto n=Number(L,index);
    if(n<0 || n>maximum || std::floor(n)!=n) throw std::invalid_argument("Graphic integer is out of range");
    return static_cast<std::uint32_t>(n);
}
R::Blend Blend(lua_State* L,int index) {
    if(lua_isnoneornil(L,index)) return R::Blend::MulAlpha;
    const auto value=CheckString(L,index);
    if(const auto blend=R::ParseBlend(value)) return *blend;
    throw std::invalid_argument("Graphic: unknown blend mode");
}
R::Paint Paint(lua_State* L,int index) {
    R::Paint result;
    if(!lua_isnoneornil(L,index)) result.color=Integer(L,index);
    result.blend=Blend(L,index+1);
    result.layer=Optional(L,index+2,0);
    return result;
}
void RememberCanvas(lua_State* L,int index,C::Handle handle) {
    lua_getfenv(L,lua_upvalueindex(1));
    lua_pushvalue(L,index); lua_rawseti(L,-2,static_cast<int>(handle.slot)+1); lua_pop(L,1);
}
void PushCanvas(lua_State* L,const std::shared_ptr<Token>& token,C::Handle handle) {
    // Weak values preserve Lua identity while a wrapper is in use, without
    // owning native attachments. Generation validation handles reused slots.
    lua_getfenv(L,lua_upvalueindex(1));
    lua_rawgeti(L,-1,static_cast<int>(handle.slot)+1);
    auto** cached=TrySlot<CanvasBox>(L,-1,CanvasType);
    if(cached && *cached && (*cached)->token==token && (*cached)->handle==handle) {
        lua_remove(L,-2); return;
    }
    lua_pop(L,2);
    auto** slot=NewSlot<CanvasBox>(L,CanvasType); *slot=new CanvasBox{token,handle};
    lua_newtable(L); lua_setfenv(L,-2);
    RememberCanvas(L,lua_gettop(L),handle);
}
C::Handle Canvas(lua_State* L,int index,Token& host) {
    auto& box=Require<CanvasBox>(L,index,CanvasType);
    if(&Live(box.token)!=&host) throw std::invalid_argument("Canvas belongs to another Graphic Context");
    return box.handle;
}
R::Image Image(lua_State* L,int index,Token& host) {
    const auto asset=std::dynamic_pointer_cast<const A::Images::ImageAsset>(Resource::AcquireImage(L,index,*host.resources));
    if(!asset || !asset->texture) throw std::invalid_argument("Resource is not a drawable Image");
    R::Image result;
    result.texture=asset->texture->Native(); result.lifetime=asset->texture;
    result.textureWidth=asset->texture->Width(); result.textureHeight=asset->texture->Height();
    result.textureLevels=asset->texture->Levels();
    result.x=asset->content.x; result.y=asset->content.y;
    result.width=asset->content.width; result.height=asset->content.height;
    const auto& options=asset->image.options;
    const auto anchor=options.anchor.value_or(A::Vec2{double(result.width)/2,double(result.height)/2});
    result.anchorX=anchor.x; result.anchorY=anchor.y;
    result.scaleX=options.scale.x; result.scaleY=options.scale.y;
    result.filter=options.filter==A::Filter::Point ? R::Filter::Point:R::Filter::Linear;
    result.address=options.address==A::Address::Repeat ? R::Address::Repeat:R::Address::Clamp;
    result.mipLevels=asset->mipLevels; result.mipExtrusion=asset->mipExtrusion;
    return result;
}
int CreateCanvasValue(lua_State* L,bool threeD) {
    Count(L,2,6);
    if(lua_gettop(L)!=2 && (threeD || lua_gettop(L)!=6)) throw std::invalid_argument("Canvas expects width,height and optionally all four 2D bounds");
    const auto w=Integer(L,1),h=Integer(L,2);
    std::optional<C::Bounds> bounds;
    if(lua_gettop(L)==6) bounds=C::Bounds{Number(L,3),Number(L,4),Number(L,5),Number(L,6)};
    auto& module=Module(L); auto& host=Live(module.token);
    auto** slot=NewSlot<CanvasBox>(L,CanvasType); // Lua allocation before native Canvas acquisition.
    auto box=std::make_unique<CanvasBox>(); box->token=module.token;
    box->handle=threeD ? host.renderer->Canvases().Create3D(w,h) : host.renderer->Canvases().Create(w,h,bounds);
    lua_newtable(L); lua_setfenv(L,-2); // Only the cached Camera proxy; no arbitrary native state.
    *slot=box.release();
    RememberCanvas(L,lua_gettop(L),(*slot)->handle);
    return 1;
}
int CreateCanvas(lua_State* L) { return CreateCanvasValue(L,false); }
int CreateCanvas3D(lua_State* L) { return CreateCanvasValue(L,true); }
int WindowCanvas(lua_State* L) {
    Count(L,0,0);
    auto& module=Module(L); auto& host=Live(module.token);
    PushCanvas(L,module.token,host.renderer->Canvases().GetWindowCanvas());
    return 1;
}
int CurrentCanvas(lua_State* L) {
    Count(L,0,0); auto& module=Module(L); auto& host=Live(module.token);
    PushCanvas(L,module.token,host.renderer->GetTargetCanvas()); return 1;
}
int GetCanvasPNG(lua_State* L) {
    Count(L,1,1);
    auto& host=Host(L);
    const auto canvas=Canvas(L,1,host);
    (void)host.renderer->Canvases().Describe(canvas); // Reject foreign/destroyed handles before readback.
    // The temporary GC shell owns native bytes if allocating the Lua binary
    // string raises an OOM/long jump. No GPU object or host pointer is retained
    // here, so it remains safe to collect after Graphic context shutdown.
    auto** slot=NewSlot<PngBox>(L,PngType); *slot=new PngBox;
    {
        auto bytes=host.renderer->GetCanvasPNG(canvas);
        if(!bytes) {
            delete *slot; *slot=nullptr; lua_pop(L,1); lua_pushnil(L); return 1;
        }
        (*slot)->bytes=std::move(*bytes);
    }
    lua_pushlstring(L,(*slot)->bytes.data(),(*slot)->bytes.size());
    delete *slot; *slot=nullptr;
    return 1;
}
int Destroy(lua_State* L) {
    Count(L,1,1); auto& box=Require<CanvasBox>(L,1,CanvasType);
    lua_pushboolean(L,Live(box.token).renderer->Canvases().Destroy(box.handle)); return 1;
}
int GetSize(lua_State* L) {
    Count(L,1,1); auto& box=Require<CanvasBox>(L,1,CanvasType);
    const auto info=Live(box.token).renderer->Canvases().Describe(box.handle);
    lua_pushnumber(L,info.width); lua_pushnumber(L,info.height); return 2;
}
int GetBounds(lua_State* L) {
    Count(L,1,1); auto& box=Require<CanvasBox>(L,1,CanvasType);
    const auto info=Live(box.token).renderer->Canvases().Describe(box.handle);
    if(info.threeD) throw std::invalid_argument("Canvas3D has a perspective projection, not 2D bounds");
    const auto b=info.bounds;
    lua_pushnumber(L,b.left); lua_pushnumber(L,b.right); lua_pushnumber(L,b.bottom); lua_pushnumber(L,b.top); return 4;
}
int Equal(lua_State* L) {
    Count(L,2,2); const auto& a=Require<CanvasBox>(L,1,CanvasType); const auto& b=Require<CanvasBox>(L,2,CanvasType);
    lua_pushboolean(L,a.token==b.token && a.handle==b.handle); return 1;
}
int SetTarget(lua_State* L) {
    Count(L,1,1); auto& host=Host(L);
    host.renderer->SetTargetCanvas(Canvas(L,1,host)); return 0;
}
int Clear(lua_State* L) { Count(L,0,1); const auto color=lua_isnoneornil(L,1) ? 0xFF000000u : Integer(L,1); Host(L).renderer->Clear(color); return 0; }
int Color(lua_State* L) {
    Count(L,4,4); (void)Host(L);
    const auto value=(Integer(L,1,255)<<24)|(Integer(L,2,255)<<16)|(Integer(L,3,255)<<8)|Integer(L,4,255);
    lua_pushnumber(L,value); return 1;
}
int RenderImage(lua_State* L) {
    Count(L,3,9);
    R::Transform transform{Number(L,2),Number(L,3),Optional(L,4,0),Optional(L,5,1),1};
    transform.scaleY=Optional(L,6,transform.scaleX);
    const auto paint=Paint(L,7); auto& host=Host(L);
    if(TrySlot<CanvasBox>(L,1,CanvasType)) host.renderer->RenderImage(Canvas(L,1,host),transform,paint);
    else host.renderer->RenderImage(Image(L,1,host),transform,paint);
    return 0;
}
int RenderRect(lua_State* L) {
    Count(L,5,8);
    const auto l=Number(L,2),r=Number(L,3),b=Number(L,4),t=Number(L,5);
    const auto paint=Paint(L,6); auto& host=Host(L);
    if(TrySlot<CanvasBox>(L,1,CanvasType)) host.renderer->RenderRect(Canvas(L,1,host),l,r,b,t,paint);
    else host.renderer->RenderRect(Image(L,1,host),l,r,b,t,paint);
    return 0;
}
// Raw tuples have no metamethods and no implicit coordinates/default vertex
// colors. Validate all Lua values before obtaining any native resource pin.
void Tuple(lua_State* L,int index,int size) {
    if(index<0 && index>LUA_REGISTRYINDEX) index=lua_gettop(L)+index+1;
    if(lua_type(L,index)!=LUA_TTABLE) throw std::invalid_argument("Graphic vertex must be a plain tuple");
    if(lua_getmetatable(L,index)) { lua_pop(L,1); throw std::invalid_argument("Graphic vertex cannot have a metatable"); }
    int count=0;
    lua_pushnil(L);
    while(lua_next(L,index)) {
        const auto key=lua_type(L,-2)==LUA_TNUMBER ? lua_tonumber(L,-2):0;
        if(key<1 || key>size || key!=std::floor(key)) {
            lua_pop(L,2); throw std::invalid_argument("Graphic vertex contains an unexpected field");
        }
        ++count; lua_pop(L,1);
    }
    if(count!=size) throw std::invalid_argument("Graphic vertex tuple is incomplete");
}
double Component(lua_State* L,int index,int field) {
    lua_rawgeti(L,index,field); const auto result=Number(L,-1); lua_pop(L,1); return result;
}
int CanvasIndex(lua_State* L) {
    Count(L,2,2); auto& box=Require<CanvasBox>(L,1,CanvasType);
    auto& renderer=*Live(box.token).renderer;
    const auto key=CheckString(L,2);
    if(key=="Camera") {
        (void)renderer.GetSpatialState(box.handle);
        lua_getfenv(L,1); lua_getfield(L,-1,"Camera");
        if(!lua_isnil(L,-1)) return 1;
        lua_pop(L,1);
        auto** slot=NewSlot<CameraBox>(L,CameraType); *slot=new CameraBox{box.token,box.handle};
        lua_pushvalue(L,-1); lua_setfield(L,-3,"Camera"); return 1;
    }
    if((key=="SetPerspective" || key=="OpenZBuffer" || key=="CloseZBuffer" || key=="SetFog") && !renderer.Canvases().Describe(box.handle).threeD) {
        lua_pushnil(L); return 1;
    }
    luaL_getmetatable(L,CanvasType); lua_getfield(L,-1,"methods"); lua_pushvalue(L,2); lua_rawget(L,-2); return 1;
}
int ReadCamera(lua_State* L) {
    Count(L,2,2); const auto& box=Require<CameraBox>(L,1,CameraType);
    const auto camera=Live(box.token).renderer->GetSpatialState(box.handle).camera;
    const auto key=CheckString(L,2);
    if(key=="x" || key=="y" || key=="z") { lua_pushnumber(L,camera.position[key=="x" ? 0:key=="y" ? 1:2]); return 1; }
    if(key!="look" && key!="up") throw std::invalid_argument("Unknown Camera property");
    const auto value=key=="look" ? camera.look:camera.up;
    lua_createtable(L,3,0);
    for(int i=0;i<3;++i) { lua_pushnumber(L,value[i]); lua_rawseti(L,-2,i+1); }
    return 1; // Direction reads are copies; write a complete vector to change state.
}
int WriteCamera(lua_State* L) {
    Count(L,3,3); auto& box=Require<CameraBox>(L,1,CameraType); auto& renderer=*Live(box.token).renderer;
    auto state=renderer.GetSpatialState(box.handle); const auto key=CheckString(L,2);
    if(key=="x" || key=="y" || key=="z") state.camera.position[key=="x" ? 0:key=="y" ? 1:2]=Number(L,3);
    else if(key=="look" || key=="up") {
        Tuple(L,3,3); const S::Point3 value{Component(L,3,1),Component(L,3,2),Component(L,3,3)};
        (key=="look" ? state.camera.look:state.camera.up)=value;
    } else throw std::invalid_argument("Unknown Camera property");
    renderer.SetSpatialState(box.handle,state); return 0;
}
int Perspective(lua_State* L) {
    Count(L,4,4); auto& box=Require<CanvasBox>(L,1,CanvasType); auto& renderer=*Live(box.token).renderer;
    auto state=renderer.GetSpatialState(box.handle); state.perspective={Number(L,2),Number(L,3),Number(L,4)};
    renderer.SetSpatialState(box.handle,state); return 0;
}
int Depth(lua_State* L,bool enabled) {
    Count(L,1,1); auto& box=Require<CanvasBox>(L,1,CanvasType); auto& renderer=*Live(box.token).renderer;
    auto state=renderer.GetSpatialState(box.handle); state.depthEnabled=enabled;
    renderer.SetSpatialState(box.handle,state); return 0;
}
int OpenDepth(lua_State* L) { return Depth(L,true); }
int CloseDepth(lua_State* L) { return Depth(L,false); }
int Fog(lua_State* L) {
    Count(L,2,6); auto& box=Require<CanvasBox>(L,1,CanvasType); auto& renderer=*Live(box.token).renderer;
    auto state=renderer.GetSpatialState(box.handle);
    if(lua_gettop(L)==2 && lua_isnil(L,2)) state.fog.reset();
    else {
        Count(L,5,6); state.fog=S::Fog{Number(L,2),Number(L,3),static_cast<int>(Integer(L,4,6)),Integer(L,5)};
        if(!lua_isnoneornil(L,6)) {
            const auto value=CheckString(L,6);
            constexpr std::string_view rgb[]{"lerp","add","sub","mul","screen"},alpha[]{"keep","mul","fade"};
            const auto split=value.find('+');
            int r=-1,a=-1;
            if(split!=std::string_view::npos) {
                for(int i=0;i<5;++i) if(value.substr(0,split)==rgb[i]) r=i;
                for(int i=0;i<3;++i) if(value.substr(split+1)==alpha[i]) a=i;
            }
            if(r<0 || a<0) throw std::invalid_argument("Fog blend expects lerp/add/sub/mul/screen + keep/mul/fade");
            state.fog->rgb=static_cast<S::FogRGB>(r); state.fog->alpha=static_cast<S::FogAlpha>(a);
        }
    }
    renderer.SetSpatialState(box.handle,state); return 0;
}
int RenderQuad(lua_State* L) {
    Count(L,5,8);
    auto& host=Host(L);
    if(host.renderer->GetTargetInfo().threeD) {
        Count(L,5,7); S::Quad3 corners;
        for(int i=0;i<4;++i) { Tuple(L,i+2,3); corners[i]={Component(L,i+2,1),Component(L,i+2,2),Component(L,i+2,3)}; }
        const auto color=lua_isnoneornil(L,6) ? 0xFFFFFFFFu : Integer(L,6); const auto blend=Blend(L,7);
        if(TrySlot<CanvasBox>(L,1,CanvasType)) host.renderer->RenderQuad3D(Canvas(L,1,host),corners,color,blend);
        else host.renderer->RenderQuad3D(Image(L,1,host),corners,color,blend);
        return 0;
    }
    core::modules::Graphic::Geometry::Quad corners{};
    for(int i=0;i<4;++i) { Tuple(L,i+2,2); corners[i]={Component(L,i+2,1),Component(L,i+2,2)}; }
    const auto paint=Paint(L,6);
    if(TrySlot<CanvasBox>(L,1,CanvasType)) host.renderer->RenderQuad(Canvas(L,1,host),corners,paint);
    else host.renderer->RenderQuad(Image(L,1,host),corners,paint);
    return 0;
}
int Render4V(lua_State* L) {
    Count(L,5,7);
    auto& host=Host(L);
    if(host.renderer->GetTargetInfo().threeD) {
        Count(L,5,6); R::QuadVertices3 vertices;
        for(int i=0;i<4;++i) {
            Tuple(L,i+2,6); auto& v=vertices[i];
            v.x=Component(L,i+2,1); v.y=Component(L,i+2,2); v.z=Component(L,i+2,3);
            v.u=Component(L,i+2,4); v.v=Component(L,i+2,5);
            lua_rawgeti(L,i+2,6); v.color=Integer(L,-1); lua_pop(L,1);
        }
        const auto blend=Blend(L,6);
        if(TrySlot<CanvasBox>(L,1,CanvasType)) host.renderer->Render4V3D(Canvas(L,1,host),vertices,blend);
        else host.renderer->Render4V3D(Image(L,1,host),vertices,blend);
        return 0;
    }
    R::QuadVertices vertices{};
    for(int i=0;i<4;++i) {
        Tuple(L,i+2,5);
        auto& v=vertices[i]; v.x=Component(L,i+2,1); v.y=Component(L,i+2,2);
        v.u=Component(L,i+2,3); v.v=Component(L,i+2,4);
        lua_rawgeti(L,i+2,5); v.color=Integer(L,-1); lua_pop(L,1);
    }
    const auto blend=Blend(L,6); const auto layer=Optional(L,7,0);
    if(TrySlot<CanvasBox>(L,1,CanvasType)) host.renderer->Render4V(Canvas(L,1,host),vertices,blend,layer);
    else host.renderer->Render4V(Image(L,1,host),vertices,blend,layer);
    return 0;
}
int RenderRibbon(lua_State* L) {
    Count(L,6,7);
    if(lua_type(L,2)!=LUA_TTABLE) throw std::invalid_argument("Ribbon points must be an array of {x,y}");
    const auto count=lua_objlen(L,2);
    if(count>core::modules::Graphic::Geometry::RibbonBuilder::maximumPoints)
        throw std::invalid_argument("Ribbon exceeds 16384 input points");
    Tuple(L,2,static_cast<int>(count));
    std::vector<core::modules::Graphic::Geometry::Point> points;
    points.reserve(count);
    for(std::size_t i=0;i<count;++i) {
        lua_rawgeti(L,2,static_cast<int>(i+1)); const auto at=lua_gettop(L);
        Tuple(L,at,2); points.push_back({Component(L,at,1),Component(L,at,2)}); lua_pop(L,1);
    }
    const auto width=Number(L,3);
    (void)CheckString(L,5); // Unlike other primitives, Ribbon requires explicit color/blend/layer.
    const R::Paint paint{Integer(L,4),Blend(L,5),Number(L,6)};
    if(!lua_isnoneornil(L,7) && lua_type(L,7)!=LUA_TBOOLEAN) throw std::invalid_argument("Ribbon closed must be a boolean");
    const bool closed=lua_toboolean(L,7)!=0; auto& host=Host(L);
    if(TrySlot<CanvasBox>(L,1,CanvasType)) host.renderer->RenderRibbon(Canvas(L,1,host),points,width,paint,closed);
    else host.renderer->RenderRibbon(Image(L,1,host),points,width,paint,closed);
    return 0;
}
R::ShaderStageData ShaderStage(ShaderParams::Stage stage) {
    R::ShaderStageData result;
    result.params.reserve(stage.params.size()); result.textures.reserve(stage.textures.size());
    for(auto& uniform:stage.params) result.params.push_back({uniform.slot,std::move(uniform.bytes)});
    for(const auto& texture:stage.textures) {
        R::TexturePart part{};
        switch(texture.part) {
        case ShaderParams::TexturePart::Color: part=R::TexturePart::Color; break;
        case ShaderParams::TexturePart::Depth: part=R::TexturePart::Depth; break;
        case ShaderParams::TexturePart::Distance: part=R::TexturePart::Distance; break;
        }
        result.textures.push_back({texture.slot,texture.texture,part});
    }
    return result;
}
int RenderShader(lua_State* L) {
    Count(L,2,4);
    // Reserve before acquiring Program/Image/Mesh pins. The remaining Lua
    // operations are raw reads, with no metamethods, callbacks or allocations.
    if(!lua_checkstack(L,24)) throw std::runtime_error("Graphic.Render Lua stack capacity exceeded");
    auto& host=Host(L);
    const auto target=host.renderer->GetTargetCanvas();
    const bool threeD=host.renderer->GetTargetInfo().threeD;
    if(threeD) Count(L,2,3);
    const double layer=threeD ? 0:Optional(L,3,0);
    const int meshIndex=threeD ? 3:4;
    if(!lua_isnoneornil(L,meshIndex) && !MeshBinding::Is(L,meshIndex))
        throw std::invalid_argument("Graphic.Render expects a Mesh in its final argument");
    const auto name=CheckString(L,1);
    if(name.empty() || name.size()>512 || name.find('\0')!=std::string_view::npos)
        throw std::invalid_argument("Graphic.Render requires a valid Shader resource name");
    const auto handle=host.resources->Find(std::string(name));
    if(!handle) throw std::invalid_argument("Shader resource not found: "+std::string(name));
    const auto asset=std::dynamic_pointer_cast<const A::ShaderAsset>(host.resources->Acquire(*handle));
    if(!asset || !asset->program) throw std::invalid_argument("Graphic.Render resource is not a ready Shader: "+std::string(name));
    R::ShaderDraw draw;
    draw.program=asset->program; draw.layer=layer;
    if(!lua_isnoneornil(L,meshIndex)) draw.mesh=MeshBinding::Pin(L,meshIndex);
    // Pack walks Lua's fields in arbitrary order. Resolve every supplied main
    // texture now, including optimized-out declarations, then order the pins
    // by its stable logical request inventory (not by physical Atlas/GPU slot).
    std::unordered_map<std::string,R::ShaderSource> sources;
    const auto resolve=[&](std::string_view field,int index,bool requires3D) {
        R::ShaderSource source;
        if(TrySlot<CanvasBox>(L,index,CanvasType)) {
            const auto canvas=Canvas(L,index,host);
            const auto info=host.renderer->Canvases().Describe(canvas);
            if(info.window) throw std::invalid_argument("WindowCanvas cannot be sampled as a texture");
            if(canvas==target) throw std::invalid_argument("current target Canvas cannot be sampled by its own draw");
            if(requires3D && !info.threeD) throw std::invalid_argument("derived texture requires a Canvas3D main input");
            source=canvas;
        } else {
            if(requires3D) throw std::invalid_argument("derived texture requires a Canvas3D main input");
            source=Image(L,index,host);
        }
        sources.emplace(std::string(field),std::move(source));
    };
    auto packed=[&] {
        try { return ShaderParams::Pack(L,2,draw.program->Metadata(),resolve); }
        catch(const std::invalid_argument& error) {
            // Metadata identifies the source path; include the logical name
            // too, since several registered shaders may share that same file.
            throw std::invalid_argument("Graphic.Render Shader '"+std::string(name)+"': "+error.what());
        }
    }();
    draw.sources.resize(packed.textures.size());
    for(std::size_t i=0;i<packed.textures.size();++i)
        if(packed.textures[i].provided) draw.sources[i]=std::move(sources.at(packed.textures[i].name));
    draw.vertex=ShaderStage(std::move(packed.vertex));
    draw.fragment=ShaderStage(std::move(packed.fragment));
    host.renderer->RenderShader(std::move(draw));
    return 0;
}
void Metatable(lua_State* L,const char* name,lua_CFunction collect,const luaL_Reg* methods=nullptr) {
    luaL_newmetatable(L,name);
    lua_pushcfunction(L,collect); lua_setfield(L,-2,"__gc");
    lua_pushstring(L,name); lua_setfield(L,-2,"__metatable");
    if(methods) {
        lua_newtable(L); luaL_register(L,nullptr,methods); lua_setfield(L,-2,"__index");
        lua_pushcfunction(L,Guard<Equal>); lua_setfield(L,-2,"__eq");
    }
    lua_pop(L,1);
}
}
struct Context::Impl {
    lua_State* state;
    std::shared_ptr<Token> token;
    Impl(lua_State* L,R::Renderer& renderer,A::Service& resources):state(L),token(std::make_shared<Token>()) {
        if(!L) throw std::invalid_argument("Graphic Context requires a Lua state");
        token->renderer=&renderer; token->resources=&resources;
    }
};
Context::Context(lua_State* L,R::Renderer& renderer,A::Service& resources):impl(std::make_unique<Impl>(L,renderer,resources)) {}
Context::~Context() { Shutdown(); }
void Context::Shutdown() noexcept { if(impl) { impl->token->renderer=nullptr; impl->token->resources=nullptr; } }
void Register(lua_State* L,Context& context) {
    if(!L || context.impl->state!=L) throw std::invalid_argument("Graphic.Register requires the Context Lua state");
    (void)Live(context.impl->token);
    const int top=lua_gettop(L);
    struct Restore { lua_State* L; int top; ~Restore() { lua_settop(L,top); } } restore{L,top};
    lua_pushlightuserdata(L,&ContextKey); lua_rawget(L,LUA_REGISTRYINDEX);
    auto** existing=TrySlot<ServiceBox>(L,-1,ServiceType);
    if(existing && *existing && (*existing)->token->renderer && (*existing)->token!=context.impl->token)
        throw std::logic_error("only one live Graphic Context may register in a Lua state");
    lua_pop(L,1);
    const luaL_Reg methods[]={{"GetSize",Guard<GetSize>},{"GetBounds",Guard<GetBounds>},{"Destroy",Guard<Destroy>},
        {"SetPerspective",Guard<Perspective>},{"OpenZBuffer",Guard<OpenDepth>},{"CloseZBuffer",Guard<CloseDepth>},{"SetFog",Guard<Fog>},{nullptr,nullptr}};
    Metatable(L,ServiceType,Guard<CollectService>);
    Metatable(L,CanvasType,Guard<CollectCanvas>,methods);
    luaL_getmetatable(L,CanvasType); lua_getfield(L,-1,"__index"); lua_setfield(L,-2,"methods");
    lua_pushcfunction(L,Guard<CanvasIndex>); lua_setfield(L,-2,"__index"); lua_pop(L,1);
    Metatable(L,CameraType,Guard<CollectCamera>);
    Metatable(L,PngType,Guard<CollectPng>);
    MeshBinding::Register(L);
    luaL_getmetatable(L,CameraType);
    lua_pushcfunction(L,Guard<ReadCamera>); lua_setfield(L,-2,"__index");
    lua_pushcfunction(L,Guard<WriteCamera>); lua_setfield(L,-2,"__newindex"); lua_pop(L,1);
    auto** slot=NewSlot<ServiceBox>(L,ServiceType); *slot=new ServiceBox{context.impl->token};
    const int service=lua_gettop(L);
    lua_newtable(L); lua_newtable(L); lua_pushliteral(L,"v"); lua_setfield(L,-2,"__mode");
    lua_setmetatable(L,-2); lua_setfenv(L,service); // Per-context weak Canvas wrapper cache.
    lua_pushlightuserdata(L,&ContextKey); lua_pushvalue(L,service); lua_rawset(L,LUA_REGISTRYINDEX);
    const luaL_Reg functions[]={
        {"Mesh",Guard<MeshBinding::Create>},
        {"Canvas",Guard<CreateCanvas>},{"Canvas3D",Guard<CreateCanvas3D>},{"GetWindowCanvas",Guard<WindowCanvas>},
        {"GetTargetCanvas",Guard<CurrentCanvas>},{"SetTargetCanvas",Guard<SetTarget>},{"GetCanvasPNG",Guard<GetCanvasPNG>},
        {"Clear",Guard<Clear>},{"Color",Guard<Color>},{"RenderImage",Guard<RenderImage>},{"RenderRect",Guard<RenderRect>},
        {"RenderQuad",Guard<RenderQuad>},{"Render4V",Guard<Render4V>},{"RenderRibbon",Guard<RenderRibbon>},
        {"Render",Guard<RenderShader>},{nullptr,nullptr}
    };
    lua_newtable(L); const int module=lua_gettop(L);
    for(const auto* f=functions;f->name;++f) {
        lua_pushvalue(L,service); lua_pushcclosure(L,f->func,1); lua_setfield(L,module,f->name);
    }
    // A new table prevents saved old modules from silently reconnecting on restart.
    lua_pushvalue(L,module); lua_setglobal(L,"Graphic");
    lua_getfield(L,LUA_REGISTRYINDEX,"_LOADED");
    if(lua_isnil(L,-1)) { lua_pop(L,1); lua_newtable(L); lua_pushvalue(L,-1); lua_setfield(L,LUA_REGISTRYINDEX,"_LOADED"); }
    if(!lua_istable(L,-1)) throw std::invalid_argument("Lua loaded-module registry must be a table");
    lua_pushvalue(L,module); lua_setfield(L,-2,"Graphic");
}
}
