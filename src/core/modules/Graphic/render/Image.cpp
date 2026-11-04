#include "Image.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace core::modules::Graphic::Render {
namespace {
constexpr std::array<std::string_view, 18> BlendNames{
    "mul+alpha", "mul+add", "mul+sub", "mul+rev", "mul+min", "mul+max", "mul+mul", "mul+screen",
    "add+alpha", "add+add", "add+sub", "add+rev", "add+min", "add+max", "add+mul", "add+screen",
    "alpha+bal", "replace"
};
static_assert(static_cast<std::size_t>(Blend::Replace) + 1 == BlendNames.size());
}
std::optional<Blend> ParseBlend(std::string_view name) noexcept {
    for (std::size_t i = 0; i < BlendNames.size(); ++i)
        if (name == BlendNames[i]) return static_cast<Blend>(i);
    return {};
}
std::string_view BlendName(Blend blend) {
    if (!IsValidBlend(blend)) throw std::invalid_argument("Invalid Image draw Blend");
    return BlendNames[static_cast<std::size_t>(blend)];
}
}

namespace core::modules::Graphic::Render::detail {
void Validate(const Image& i, Paint p) {
    if (!i.texture || !i.lifetime || !i.width || !i.height || !i.textureWidth || !i.textureHeight ||
        std::uint64_t(i.x)+i.width>i.textureWidth || std::uint64_t(i.y)+i.height>i.textureHeight)
        throw std::invalid_argument("RenderImage requires a live backing and valid Image region");
    if (!std::isfinite(i.insetX) || !std::isfinite(i.insetY) || !std::isfinite(i.logicalWidth) ||
        !std::isfinite(i.logicalHeight) || i.insetX < 0 || i.insetY < 0 || i.logicalWidth < 0 || i.logicalHeight < 0 ||
        i.insetX + i.LogicalWidth() > i.width || i.insetY + i.LogicalHeight() > i.height)
        throw std::invalid_argument("RenderImage logical region exceeds its physical pixel coverage");
    if(!i.textureLevels || i.textureLevels>static_cast<std::uint32_t>(std::bit_width(std::max(i.textureWidth,i.textureHeight))) ||
        (i.mipLevels && i.textureLevels!=1))
        throw std::invalid_argument("RenderImage has invalid physical mip metadata");
    if (!std::isfinite(i.anchorX) || !std::isfinite(i.anchorY) || !std::isfinite(i.scaleX) ||
        !std::isfinite(i.scaleY) || !std::isfinite(p.layer))
        throw std::invalid_argument("RenderImage metrics and Layer must be finite");
    if ((i.filter!=Filter::Point && i.filter!=Filter::Linear) || (i.address!=Address::Clamp && i.address!=Address::Repeat) ||
        (i.alpha!=Alpha::Straight && i.alpha!=Alpha::Premultiplied) || !IsValidBlend(p.blend))
        throw std::invalid_argument("Invalid Image sampling/alpha or draw Blend");
    if (i.mipLevels) {
        const auto levels=static_cast<std::uint32_t>(std::bit_width(std::max(i.width,i.height)));
        if (i.mipLevels!=levels)
            throw std::invalid_argument("Packed Image mip levels must describe its complete logical mip chain");
        // Validate the entire allocation, not only the base content rectangle.
        // Use wide arithmetic so invalid native snapshots cannot wrap a bounds
        // check or let a later level read another Image's pixels.
        const std::uint64_t padding=i.mipExtrusion;
        if (i.x<padding || i.y<padding || std::uint64_t(i.x)+i.width+padding>i.textureWidth)
            throw std::invalid_argument("Packed Image mip strip has invalid horizontal bounds or extrusion");
        std::uint64_t bottom=std::uint64_t(i.y)-padding;
        for (std::uint32_t level=0;level<i.mipLevels;++level)
            bottom+=std::max(1u,i.height>>level)+2*padding;
        if (bottom>i.textureHeight)
            throw std::invalid_argument("Packed Image mip strip exceeds its texture height");
    }
}
std::array<std::array<double,2>,4> ImageCorners(const Image& i, Transform t) {
    if (!std::isfinite(t.x) || !std::isfinite(t.y) || !std::isfinite(t.rotation) ||
        !std::isfinite(t.scaleX) || !std::isfinite(t.scaleY))
        throw std::invalid_argument("RenderImage transform must be finite");
    const double angle=std::remainder(t.rotation,360.0)*(std::numbers::pi/180.0);
    const double c=std::cos(angle),s=std::sin(angle);
    const double left=-i.anchorX*i.scaleX*t.scaleX, right=(i.LogicalWidth()-i.anchorX)*i.scaleX*t.scaleX;
    const double top=i.anchorY*i.scaleY*t.scaleY, bottom=(i.anchorY-i.LogicalHeight())*i.scaleY*t.scaleY;
    std::array<std::array<double,2>,4> result{{{right,top},{left,top},{left,bottom},{right,bottom}}};
    for (auto& p:result) { const auto x=p[0],y=p[1]; p={t.x+c*x-s*y,t.y+s*x+c*y}; }
    return result;
}
}
