#include "TextBinding.h"
#include "Graphic.h"
#include "lua_bind/Guard.h"
#include "core/modules/Graphic/text/Text.h"
#include "core/modules/Resource/Resource.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace lua_bind::modules::Graphic::TextBinding {
namespace {
namespace T=core::modules::Graphic::Text;
namespace R=core::modules::Resource;
void Count(lua_State* L,int n,int maximum=-1) {
    if(maximum<0)maximum=n;
    if(lua_gettop(L)<n || lua_gettop(L)>maximum)throw std::invalid_argument("Graphic text: invalid argument count");
    if(!lua_checkstack(L,4))throw std::runtime_error("Graphic text: Lua stack capacity exceeded");
}
double Number(lua_State* L,int i) {
    if(lua_type(L,i)!=LUA_TNUMBER || !std::isfinite(lua_tonumber(L,i)))throw std::invalid_argument("Graphic text: expected finite number");
    return lua_tonumber(L,i);
}
std::uint32_t Color(lua_State* L,int i) {
    const double value=Number(L,i);
    if(value<0 || value>std::numeric_limits<std::uint32_t>::max() || std::floor(value)!=value)throw std::invalid_argument("Graphic text: color must be uint32 ARGB");
    return static_cast<std::uint32_t>(value);
}
T::State& State(lua_State* L){return *ResolveDrawingTarget(L,false).text;}
void String(lua_State* L,std::string_view value){lua_pushlstring(L,value.data(),value.size());}
T::FontRef Font(lua_State* L,int index,R::Service& resources) {
    const auto name=CheckString(L,index);
    if(name.empty() || name.find('\0')!=name.npos)throw std::invalid_argument("Graphic text: invalid Font name");
    const auto handle=resources.Find(std::string(name));
    if(!handle || resources.GetInfo(*handle).id.kind!=R::Kind::Font)throw std::invalid_argument("Graphic text: expected a ready Font resource name");
    auto asset=std::dynamic_pointer_cast<const R::Fonts::FontAsset>(resources.Acquire(*handle));
    if(!asset)throw std::invalid_argument("Graphic text: resource is not a Font");
    return {*handle,std::move(asset)};
}
int SetAnchor(lua_State* L){Count(L,1);const auto value=T::ParseAnchor(CheckString(L,1));State(L).style.anchor=value;return 0;}
int GetAnchor(lua_State* L){Count(L,0);String(L,T::Name(State(L).style.anchor));return 1;}
int SetAlign(lua_State* L){Count(L,1);const auto value=T::ParseAlign(CheckString(L,1));State(L).style.align=value;return 0;}
int GetAlign(lua_State* L){Count(L,0);String(L,T::Name(State(L).style.align));return 1;}
int SetMode(lua_State* L){Count(L,1);const auto value=T::ParseWritingMode(CheckString(L,1));State(L).style.writingMode=value;return 0;}
int GetMode(lua_State* L){Count(L,0);String(L,T::Name(State(L).style.writingMode));return 1;}
int SetLetter(lua_State* L){Count(L,1);const auto value=Number(L,1);State(L).style.letterSpacing=value;return 0;}
int GetLetter(lua_State* L){Count(L,0);lua_pushnumber(L,State(L).style.letterSpacing);return 1;}
int SetLine(lua_State* L){Count(L,1);const auto value=Number(L,1);State(L).style.lineSpacing=value;return 0;}
int GetLine(lua_State* L){Count(L,0);lua_pushnumber(L,State(L).style.lineSpacing);return 1;}
int SetOutline(lua_State* L) {
    Count(L,1,2);const auto width=Number(L,1);if(width<0)throw std::invalid_argument("Graphic text: outline width must be nonnegative");
    std::optional<std::uint32_t> color;if(lua_gettop(L)==2)color=Color(L,2);
    auto& style=State(L).style;style.outlineWidth=width;if(color)style.outlineColor=*color;return 0;
}
int GetOutline(lua_State* L){Count(L,0);const auto& s=State(L).style;lua_pushnumber(L,s.outlineWidth);lua_pushnumber(L,s.outlineColor);return 2;}
int Width(lua_State* L) {
    Count(L,3);auto host=ResolveDrawingTarget(L,false);
    const auto layout=host.text->engine.Layout(Font(L,1,*host.resources),CheckString(L,2),Number(L,3),host.text->style);
    lua_pushnumber(L,layout->bounds.Width());return 1;
}
int Height(lua_State* L) {
    Count(L,3);auto host=ResolveDrawingTarget(L,false);
    const auto layout=host.text->engine.Layout(Font(L,1,*host.resources),CheckString(L,2),Number(L,3),host.text->style);
    lua_pushnumber(L,layout->bounds.Height());return 1;
}
int Wrap(lua_State* L) {
    Count(L,5);auto host=ResolveDrawingTarget(L,false);
    const auto value=host.text->engine.Wrap(Font(L,1,*host.resources),CheckString(L,2),Number(L,3),host.text->style,Number(L,4),Number(L,5));
    if(value)String(L,*value);else lua_pushnil(L);return 1;
}
int Render(lua_State* L,bool explicitTarget) {
    auto host=ResolveDrawingTarget(L,explicitTarget);Count(L,8);
    const double x=Number(L,3),y=Number(L,4),size=Number(L,5),layer=Number(L,8);
    const auto color=Color(L,6);const auto blend=CheckString(L,7);
    // Empty strings and minimized windows still validate active frame and 2D
    // target. Width/Height/Wrap never make this GPU/frame-dependent query.
    ValidateTextTarget(host,blend);
    auto layout=host.text->engine.Layout(Font(L,1,*host.resources),CheckString(L,2),size,host.text->style);
    SubmitText(host,std::move(layout),x,y,color,blend,layer);return 0;
}
int RenderModule(lua_State* L){return Render(L,false);}
int RenderCanvas(lua_State* L){return Render(L,true);}
const luaL_Reg functions[]={
    {"SetTextAnchor",Guard<SetAnchor>},{"GetTextAnchor",Guard<GetAnchor>},
    {"SetTextAlign",Guard<SetAlign>},{"GetTextAlign",Guard<GetAlign>},
    {"SetTextWritingMode",Guard<SetMode>},{"GetTextWritingMode",Guard<GetMode>},
    {"SetTextLetterSpacing",Guard<SetLetter>},{"GetTextLetterSpacing",Guard<GetLetter>},
    {"SetTextLineSpacing",Guard<SetLine>},{"GetTextLineSpacing",Guard<GetLine>},
    {"SetTextOutline",Guard<SetOutline>},{"GetTextOutline",Guard<GetOutline>},
    {"RenderText",Guard<RenderModule>},{"GetTextWidth",Guard<Width>},{"GetTextHeight",Guard<Height>},{"GenTextWrap",Guard<Wrap>},{nullptr,nullptr}
};
}
const luaL_Reg* Functions() noexcept{return functions;}
int RenderTextToCanvas(lua_State* L){return Guard<RenderCanvas>(L);}
}
