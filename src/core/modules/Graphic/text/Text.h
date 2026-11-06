#pragma once
#include "core/modules/Resource/Font.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// CPU text coordinates are X-right/Y-down. Rendering maps this immutable local
// geometry into its target's logical coordinates. This module never owns a GPU.
namespace core::modules::Graphic::Text {
enum class Anchor { TopLeft, Top, TopRight, Left, Center, Right, BottomLeft, Bottom, BottomRight };
enum class Align { Start, Center, End, Left, Right, Top, Bottom };
enum class WritingMode { Horizontal, VerticalRL, VerticalLR };
Anchor ParseAnchor(std::string_view);
Align ParseAlign(std::string_view);
WritingMode ParseWritingMode(std::string_view);
std::string_view Name(Anchor);
std::string_view Name(Align);
std::string_view Name(WritingMode);
struct Style {
    Anchor anchor{Anchor::Center};
    Align align{Align::Start};
    WritingMode writingMode{WritingMode::Horizontal};
    double letterSpacing{}, lineSpacing{}, outlineWidth{};
    std::uint32_t outlineColor{0xff000000};
};
struct FontRef {
    Resource::Handle generation;
    std::shared_ptr<const Resource::Fonts::FontAsset> asset;
};
struct Bounds {
    double left{}, top{}, right{}, bottom{};
    double Width() const noexcept { return right-left; }
    double Height() const noexcept { return bottom-top; }
};
struct Point { double x{}, y{}; };
struct Glyph {
    std::uint32_t id{}, cluster{}; // cluster is a normalized UTF-8 source byte offset.
    double x{}, y{}; // glyph origin in logical coordinates, before anchor translation
    bool clockwise{};
    bool unsafeBefore{};
};
struct Line {
    Bounds bounds;
    std::size_t firstGlyph{}, glyphCount{};
    bool rightToLeft{};
};
struct LayoutResult {
    FontRef font;
    Style style;
    double size{}, defaultPitch{};
    std::string text; // CR-normalized UTF-8; tabs and embedded NUL retained.
    Bounds bounds;
    std::vector<Glyph> glyphs;
    std::vector<Line> lines;
    Point AnchorPoint() const noexcept;
};
struct Limits {
    std::size_t maxTextBytes{1024*1024}, maxGlyphs{262144}, maxLines{16384};
    std::size_t maxWrapCandidates{65536};
    std::size_t maxWrapShapeBytes{64*1024*1024};
    std::size_t maxGlyphCacheBytes{64*1024*1024}, maxGlyphBitmapBytes{16*1024*1024};
    std::uint32_t maxGlyphDimension{8192};
    std::size_t maxGlyphCacheEntries{8192};
    // Optional content LRU: accounted retained data, font bytes and node storage.
    // Either zero disables caching; valid layouts larger than the cap bypass it.
    std::size_t maxLayoutCacheBytes{32*1024*1024}, maxLayoutCacheEntries{512};
};
// Cache misses own their mutable FreeType/HarfBuzz/Unicode objects. Engine is safe
// for independent concurrent CPU calls; returned objects only contain owned data.
class Engine final {
public:
    explicit Engine(Limits = {});
    ~Engine();
    Engine(const Engine&)=delete;
    Engine& operator=(const Engine&)=delete;
    std::shared_ptr<const LayoutResult> Layout(FontRef, std::string_view, double size, const Style&) const;
    std::optional<std::string> Wrap(FontRef, std::string_view, double size, const Style&, double width, double height) const;
private:
    struct LayoutCache;
    Limits limits;
    std::unique_ptr<LayoutCache> cache;
};
// Owned by the Graphic Lua context, shared by module/object entry points and
// coroutines. It is never a process-global or a Canvas-private style.
struct State { Style style; Engine engine; };
struct GlyphBitmap {
    // Integer raster rectangle relative to the glyph origin. All masks share
    // this exact grid; masks are coverage bytes. Upload RGB=255, A=coverage.
    std::uint32_t width{}, height{};
    int left{}, top{};
    double rasterScale{1};
    std::vector<std::uint8_t> fill, ring;
    std::shared_ptr<const void> allocation; // accounts retained masks after cache eviction
};
struct PreparedGlyph {
    std::shared_ptr<const GlyphBitmap> bitmap;
    double x{}, y{}; // logical glyph origin, same as LayoutResult
};
struct PreparedText {
    std::shared_ptr<const LayoutResult> layout;
    std::vector<PreparedGlyph> glyphs; // outlines all first, then fills, in this order
};
// Bounded CPU mask cache. External PreparedText references pin immutable masks;
// live entries are never overwritten. GPU page/upload/fence ownership belongs to
// the renderer and must pin PreparedText through the command's completion.
class GlyphCache final {
public:
    explicit GlyphCache(Limits = {});
    ~GlyphCache();
    GlyphCache(const GlyphCache&)=delete;
    GlyphCache& operator=(const GlyphCache&)=delete;
    std::shared_ptr<const PreparedText> Prepare(std::shared_ptr<const LayoutResult>, double rasterScale=1);
    void Clear();
    std::size_t Bytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
std::string_view Versions() noexcept;
}
