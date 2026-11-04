#pragma once
#include "Text.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <map>
namespace core::modules::Graphic::Text::detail {
struct Face final {
    explicit Face(FontRef);
    ~Face();
    Face(const Face&)=delete;
    Face& operator=(const Face&)=delete;
    FontRef font;
    FT_Library library{};
    FT_Face face{};
    hb_blob_t* blob{};
    hb_face_t* hbFace{};
    hb_font_t* hbFont{};
    double units{};
    void Check(FT_Error) const;
    std::optional<Bounds> Ink(std::uint32_t glyph,double size,bool clockwise,double outline);
    bool Rotate(std::uint32_t cp,std::uint32_t script);
    std::shared_ptr<const GlyphBitmap> Raster(std::uint32_t glyph,double size,bool clockwise,double outline,double rasterScale,const Limits&);
private:
    struct Memory;
    std::unique_ptr<Memory> memory;
    std::map<std::uint32_t,bool> verticalRotation;
    void Release() noexcept;
};
}
