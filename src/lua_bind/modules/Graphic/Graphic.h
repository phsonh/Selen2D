#pragma once
#include <lua.hpp>
#include <memory>
#include <optional>
#include <string_view>
#include "core/modules/Graphic/canvas/Canvas.h"

namespace core::modules::Graphic::Render { class Renderer; }
namespace core::modules::Resource { class Service; }
namespace core::modules::Graphic::Text { struct State; struct LayoutResult; }
namespace lua_bind::modules::Graphic {
// Private binding bridge shared by primitive and text callbacks. Explicit
// targets consume their Lua self argument, never mutate the module default.
struct DrawingTarget {
    core::modules::Graphic::Render::Renderer* renderer{};
    core::modules::Resource::Service* resources{};
    core::modules::Graphic::Text::State* text{};
    std::optional<core::modules::Graphic::Canvas::Handle> output;
    std::shared_ptr<void> contextLifetime;
};
DrawingTarget ResolveDrawingTarget(lua_State*, bool explicitTarget);
void ValidateTextTarget(const DrawingTarget&, std::string_view blend);
void SubmitText(const DrawingTarget&, std::shared_ptr<const core::modules::Graphic::Text::LayoutResult>,
    double x, double y, std::uint32_t color, std::string_view blend, double layer);
// Borrowed host services; close before resource/renderer destruction. Lua GC
// only releases wrapper storage, never decides Canvas or GPU residency.
class Context final {
public:
    Context(lua_State*, core::modules::Graphic::Render::Renderer&, core::modules::Resource::Service&);
    ~Context();
    Context(const Context&)=delete;
    Context& operator=(const Context&)=delete;
    void Shutdown() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    friend void Register(lua_State*, Context&);
};
void Register(lua_State*, Context&);
}
