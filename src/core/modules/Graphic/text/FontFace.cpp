#include "FontFace.h"
#include "Unicode.h"
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_BBOX_H
#include FT_GLYPH_H
#include FT_STROKER_H
#include <hb-ot.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace core::modules::Graphic::Text::detail {
struct Face::Memory {
    struct alignas(std::max_align_t) Block { std::size_t size; };
    static constexpr std::size_t limit=32*1024*1024;
    std::size_t used{};
    FT_MemoryRec_ callbacks{this,Allocate,Free,Reallocate};
    static void* Allocate(FT_Memory m,long n) noexcept {
        auto& self=*static_cast<Memory*>(m->user);
        if(n<=0 || static_cast<std::size_t>(n)>limit-sizeof(Block) || static_cast<std::size_t>(n)+sizeof(Block)>limit-self.used) return nullptr;
        auto* p=static_cast<Block*>(std::malloc(sizeof(Block)+n)); if(!p) return nullptr;
        p->size=n; self.used+=sizeof(Block)+n; return p+1;
    }
    static void Free(FT_Memory m,void* p) noexcept {
        if(!p)return; auto* b=static_cast<Block*>(p)-1;
        static_cast<Memory*>(m->user)->used-=sizeof(Block)+b->size; std::free(b);
    }
    static void* Reallocate(FT_Memory m,long,long n,void* p) noexcept {
        if(n<=0){Free(m,p);return nullptr;}
        auto* q=Allocate(m,n); if(!q)return nullptr;
        if(p){std::memcpy(q,p,std::min(static_cast<Block*>(p)[-1].size,static_cast<std::size_t>(n)));Free(m,p);} return q;
    }
};
Face::Face(FontRef value):font(std::move(value)),memory(std::make_unique<Memory>()) {
    if(!font.asset || !font.asset->bytes || font.asset->bytes->empty() || font.asset->bytes->size()>32*1024*1024)
        throw std::invalid_argument("Text requires a valid ready Font resource");
    try {
        Check(FT_New_Library(&memory->callbacks,&library)); FT_Add_Default_Modules(library);
        Check(FT_New_Memory_Face(library,reinterpret_cast<const FT_Byte*>(font.asset->bytes->data()),static_cast<FT_Long>(font.asset->bytes->size()),0,&face));
        if(!FT_IS_SFNT(face) || !FT_IS_SCALABLE(face) || face->num_faces!=1 || !face->units_per_EM)
            throw std::invalid_argument("Text supports standalone scalable TTF/OTF outline fonts");
        if(FT_HAS_COLOR(face)) throw std::invalid_argument("Text does not support color font layers");
        if(FT_HAS_MULTIPLE_MASTERS(face)) throw std::invalid_argument("Text does not support variable font axes");
        Check(FT_Select_Charmap(face,FT_ENCODING_UNICODE));
        units=face->units_per_EM;
        blob=hb_blob_create(font.asset->bytes->data(),static_cast<unsigned>(font.asset->bytes->size()),HB_MEMORY_MODE_READONLY,nullptr,nullptr);
        hbFace=hb_face_create(blob,0); hbFont=hb_font_create(hbFace);
        if(!blob || !hbFace || !hbFont || !hb_face_get_glyph_count(hbFace)) throw std::runtime_error("Text font shaping initialization failed");
        hb_ot_font_set_funcs(hbFont);
        // 1/64 design-unit positioning preserves fractional size without tying
        // layout to raster DPI, hinted advances, or a quantized FT face size.
        hb_font_set_scale(hbFont,face->units_per_EM*64,face->units_per_EM*64);
    } catch(...) {Release();throw;}
}
void Face::Release() noexcept {
    if(hbFont)hb_font_destroy(hbFont); if(hbFace)hb_face_destroy(hbFace); if(blob)hb_blob_destroy(blob);
    if(face)FT_Done_Face(face); if(library)FT_Done_Library(library);
    hbFont=nullptr;hbFace=nullptr;blob=nullptr;face=nullptr;library=nullptr;
}
Face::~Face(){Release();}
void Face::Check(FT_Error error) const {
    if(error) throw std::runtime_error("Text FreeType preparation failed ("+std::to_string(error)+")");
}
std::optional<Bounds> Face::Ink(std::uint32_t glyph,double size,bool clockwise,double outline) {
    Check(FT_Load_Glyph(face,glyph,FT_LOAD_NO_SCALE|FT_LOAD_NO_HINTING|FT_LOAD_NO_BITMAP|FT_LOAD_IGNORE_TRANSFORM));
    if(face->glyph->format!=FT_GLYPH_FORMAT_OUTLINE) throw std::runtime_error("Text glyph does not contain a supported outline");
    if(!face->glyph->outline.n_points) return std::nullopt;
    FT_BBox box{}; Check(FT_Outline_Get_BBox(&face->glyph->outline,&box));
    const double s=size/units;
    if(clockwise) return Bounds{box.yMin*s-outline,box.xMin*s-outline,box.yMax*s+outline,box.xMax*s+outline};
    return Bounds{box.xMin*s-outline,-box.yMax*s-outline,box.xMax*s+outline,-box.yMin*s+outline};
}
bool Face::Rotate(std::uint32_t cp,std::uint32_t script) {
    // Use the pinned Unicode orientation for every script. Mongolian letters
    // are R: OpenType fonts store their horizontal forms, so shape the joined
    // run horizontally and rotate the complete result for vertical layout.
    const auto orientation=VerticalOrientation(cp);
    if(orientation!=2) return orientation==1;
    if(auto it=verticalRotation.find(cp);it!=verticalRotation.end())return it->second;
    auto shape=[&](hb_direction_t direction){
        std::unique_ptr<hb_buffer_t,decltype(&hb_buffer_destroy)> buffer(hb_buffer_create(),hb_buffer_destroy);
        auto* b=buffer.get(); if(!b)throw std::bad_alloc();
        hb_buffer_add_utf32(b,&cp,1,0,1); hb_buffer_set_direction(b,direction);
        hb_buffer_set_script(b,static_cast<hb_script_t>(script)); hb_buffer_set_language(b,hb_language_from_string("und",3));
        if(!hb_shape_full(hbFont,b,nullptr,0,nullptr) || !hb_buffer_allocation_successful(b))
            throw std::runtime_error("Text vertical substitution preparation failed");
        unsigned count{}; auto* info=hb_buffer_get_glyph_infos(b,&count);
        return count==1 ? info[0].codepoint:0u;
    };
    const bool rotate=shape(HB_DIRECTION_TTB)==shape(HB_DIRECTION_LTR);
    verticalRotation.emplace(cp,rotate);return rotate;
}
namespace {
struct GlyphOwner { FT_Glyph value{}; ~GlyphOwner(){if(value)FT_Done_Glyph(value);} };
struct Stroker { FT_Stroker value{}; ~Stroker(){if(value)FT_Stroker_Done(value);} };
}
std::shared_ptr<const GlyphBitmap> Face::Raster(std::uint32_t glyph,double size,bool clockwise,double outline,double rasterScale,const Limits& limits) {
    auto result=std::make_shared<GlyphBitmap>();result->rasterScale=rasterScale;
    const auto ink=Ink(glyph,size,clockwise,outline);
    if(!ink) return result;
    if((ink->Width()*rasterScale+4)>limits.maxGlyphDimension || (ink->Height()*rasterScale+4)>limits.maxGlyphDimension)
        throw std::length_error("Text glyph raster dimensions exceed limit");
    GlyphOwner fill,expanded; Check(FT_Get_Glyph(face->glyph,&fill.value));
    const auto factor=size*rasterScale/units*64*65536;
    if(!std::isfinite(factor) || factor>std::numeric_limits<FT_Fixed>::max())
        throw std::length_error("Text glyph raster scale cannot be represented");
    const auto scaled=static_cast<FT_Fixed>(std::llround(factor));
    FT_Matrix matrix=clockwise ? FT_Matrix{0,scaled,-scaled,0}:FT_Matrix{scaled,0,0,scaled};
    Check(FT_Glyph_Transform(fill.value,&matrix,nullptr));
    if(outline>0) {
        Check(FT_Glyph_Copy(fill.value,&expanded.value));
        const double radius=outline*rasterScale*64;
        if(!std::isfinite(radius) || radius>std::numeric_limits<FT_Fixed>::max()) throw std::length_error("Text outline raster width exceeds limit");
        Stroker stroker;Check(FT_Stroker_New(library,&stroker.value));
        FT_Stroker_Set(stroker.value,static_cast<FT_Fixed>(std::llround(radius)),FT_STROKER_LINECAP_ROUND,FT_STROKER_LINEJOIN_ROUND,0);
        Check(FT_Glyph_StrokeBorder(&expanded.value,stroker.value,false,true));
    }
    Check(FT_Glyph_To_Bitmap(&fill.value,FT_RENDER_MODE_NORMAL,nullptr,true));
    if(expanded.value) Check(FT_Glyph_To_Bitmap(&expanded.value,FT_RENDER_MODE_NORMAL,nullptr,true));
    const auto f=reinterpret_cast<FT_BitmapGlyph>(fill.value);
    const auto e=expanded.value ? reinterpret_cast<FT_BitmapGlyph>(expanded.value):f;
    const int left=std::min(f->left,e->left),top=std::max(f->top,e->top);
    const int right=std::max(f->left+static_cast<int>(f->bitmap.width),e->left+static_cast<int>(e->bitmap.width));
    const int bottom=std::min(f->top-static_cast<int>(f->bitmap.rows),e->top-static_cast<int>(e->bitmap.rows));
    if(right<left || top<bottom || static_cast<unsigned>(right-left)>limits.maxGlyphDimension || static_cast<unsigned>(top-bottom)>limits.maxGlyphDimension)
        throw std::length_error("Text glyph raster dimensions exceed limit");
    result->width=right-left;result->height=top-bottom;result->left=left;result->top=-top;
    const auto pixels=static_cast<std::size_t>(result->width)*result->height;
    if(pixels>limits.maxGlyphBitmapBytes/2) throw std::length_error("Text glyph raster byte limit exceeded");
    result->fill.assign(pixels,0);result->ring.assign(pixels,0);
    const auto copy=[&](FT_BitmapGlyph input,std::vector<std::uint8_t>& output){
        const auto& b=input->bitmap;
        if(b.pixel_mode!=FT_PIXEL_MODE_GRAY && b.width && b.rows) throw std::runtime_error("Text raster is not a grayscale outline mask");
        for(unsigned y=0;y<b.rows;++y) {
            const auto row=b.buffer+(b.pitch>=0 ? y*static_cast<std::size_t>(b.pitch):(b.rows-1-y)*static_cast<std::size_t>(-b.pitch));
            const auto offset=static_cast<std::size_t>(top-input->top+y)*result->width+input->left-left;
            for(unsigned x=0;x<b.width;++x) output[offset+x]=row[x];
        }
    };
    copy(f,result->fill);
    if(expanded.value) {copy(e,result->ring);for(std::size_t i=0;i<pixels;++i)result->ring[i]=result->ring[i]>result->fill[i] ? result->ring[i]-result->fill[i]:0;}
    return result;
}
}
