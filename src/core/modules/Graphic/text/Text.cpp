#include "Text.h"
#include "FontFace.h"
#include "Unicode.h"
#include FT_TRUETYPE_TABLES_H
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace core::modules::Graphic::Text {
namespace {
constexpr std::array<std::string_view,9> anchors{"top_left","top","top_right","left","center","right","bottom_left","bottom","bottom_right"};
constexpr std::array<std::string_view,7> aligns{"start","center","end","left","right","top","bottom"};
constexpr std::array<std::string_view,3> modes{"horizontal","vertical_rl","vertical_lr"};
template<class E,std::size_t N>E Parse(std::string_view s,const std::array<std::string_view,N>& names) {
    const auto it=std::find(names.begin(),names.end(),s);
    if(it==names.end())throw std::invalid_argument("Text: invalid state enumeration");return static_cast<E>(it-names.begin());
}
template<class E,std::size_t N>std::string_view EnumName(E value,const std::array<std::string_view,N>& names) {
    const auto i=static_cast<std::size_t>(value);if(i>=N)throw std::invalid_argument("Text: invalid native state enumeration");return names[i];
}
void Finite(double n,const char* what){if(!std::isfinite(n))throw std::invalid_argument(what);}
void Validate(double size,const Style& s) {
    Finite(size,"Text size must be finite");if(size<=0)throw std::invalid_argument("Text size must be positive");
    Name(s.anchor);Name(s.align);Name(s.writingMode);
    Finite(s.letterSpacing,"Text letter spacing must be finite");Finite(s.lineSpacing,"Text line spacing must be finite");
    Finite(s.outlineWidth,"Text outline width must be finite");if(s.outlineWidth<0)throw std::invalid_argument("Text outline width must be nonnegative");
    if(s.writingMode==WritingMode::Horizontal ? (s.align==Align::Top || s.align==Align::Bottom):(s.align==Align::Left || s.align==Align::Right))
        throw std::invalid_argument("Text alignment is incompatible with writing mode");
}
void Union(Bounds& a,const Bounds& b){a.left=std::min(a.left,b.left);a.top=std::min(a.top,b.top);a.right=std::max(a.right,b.right);a.bottom=std::max(a.bottom,b.bottom);}
void Move(Bounds& b,double x,double y){b.left+=x;b.right+=x;b.top+=y;b.bottom+=y;}
void ValidBounds(const Bounds& b) {
    if(!std::isfinite(b.left) || !std::isfinite(b.right) || !std::isfinite(b.top) || !std::isfinite(b.bottom) ||
        !std::isfinite(b.Width()) || !std::isfinite(b.Height())) throw std::overflow_error("Text layout exceeds finite logical coordinates");
}
struct Buffer { hb_buffer_t* value{hb_buffer_create()};~Buffer(){hb_buffer_destroy(value);} };
struct Shaped {
    std::uint32_t id{},cluster{};double x{},y{},advance{};bool clockwise{},unsafe{};
};
struct Metrics {double before{},after{},pitch{};};
Metrics GetMetrics(detail::Face& face,double size,bool vertical) {
    const double s=size/face.units;
    if(!vertical) {
        const double ascent=face.face->ascender,descent=face.face->descender;
        const double frame=ascent>descent ? (ascent-descent)*s:size;
        return {ascent>descent ? -ascent*s:-size,ascent>descent ? -descent*s:0,
            face.face->height>0 ? face.face->height*s:frame};
    }
    if(FT_HAS_VERTICAL(face.face)) {
        const auto* metrics=static_cast<const TT_VertHeader*>(FT_Get_Sfnt_Table(face.face,ft_sfnt_vhea));
        if(metrics && metrics->Ascender>metrics->Descender) {
            const double pitch=(metrics->Ascender-metrics->Descender+metrics->Line_Gap)*s;
            if(pitch>0)return {metrics->Descender*s,metrics->Ascender*s,pitch};
        }
    }
    // Fonts without a usable vhea use a centered one-em column frame/pitch.
    // Per-glyph vertical origins and advances still come from HarfBuzz's fixed
    // OpenType metrics fallback, never from a character chosen at runtime.
    return {-size/2,size/2,size};
}
std::vector<Shaped> Shape(detail::Face& face,const detail::UnicodeText& text,std::size_t begin,std::size_t end,bool vertical,bool& rtl,std::size_t remaining) {
    const auto analysis=detail::Analyze(text.points,begin,end);rtl=analysis.rtl;
    std::vector<Shaped> result;
    for(const auto& run:analysis.runs) {
        struct Part{std::size_t begin,end;bool rotate;};std::vector<Part> parts;
        for(auto i=run.begin;i<run.end;) {
            const bool rotate=vertical && face.Rotate(text.points[i],run.script);
            auto last=i+1;
            while(last<run.end && (!vertical || !text.grapheme[last] || face.Rotate(text.points[last],run.script)==rotate))++last;
            parts.push_back({i,last,rotate});i=last;
        }
        if(run.rtl)std::reverse(parts.begin(),parts.end());
        for(const auto& part:parts) {
            Buffer b;if(!b.value)throw std::bad_alloc();
            hb_buffer_set_direction(b.value,vertical && !part.rotate ? HB_DIRECTION_TTB:(run.rtl ? HB_DIRECTION_RTL:HB_DIRECTION_LTR));
            hb_buffer_set_script(b.value,static_cast<hb_script_t>(run.script));
            hb_buffer_set_language(b.value,hb_language_from_string("und",3));
            hb_buffer_set_cluster_level(b.value,HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
            hb_buffer_set_flags(b.value,HB_BUFFER_FLAG_PRODUCE_UNSAFE_TO_CONCAT);
            hb_buffer_add_utf32(b.value,text.points.data(),static_cast<int>(text.points.size()),static_cast<unsigned>(part.begin),static_cast<int>(part.end-part.begin));
            if(!hb_shape_full(face.hbFont,b.value,nullptr,0,nullptr) || !hb_buffer_allocation_successful(b.value))
                throw std::runtime_error("Text shaping failed or exceeded its allocation/work limits");
            unsigned count{};const auto* info=hb_buffer_get_glyph_infos(b.value,&count);const auto* pos=hb_buffer_get_glyph_positions(b.value,nullptr);
            if(count>remaining-result.size())throw std::length_error("Text glyph count limit exceeded");
            for(unsigned n=0;n<count;++n) {
                // Default-ignorable controls still participate in shaping/bidi
                // but their synthetic zero-advance space is not a spacing unit.
                if(info[n].cluster<text.points.size() && !pos[n].x_advance && !pos[n].y_advance &&
                    hb_unicode_general_category(hb_unicode_funcs_get_default(),text.points[info[n].cluster])==HB_UNICODE_GENERAL_CATEGORY_FORMAT)continue;
                const double factor=1.0/(face.units*64);
                Shaped item;item.id=info[n].codepoint;item.cluster=info[n].cluster;item.clockwise=part.rotate;
                item.unsafe=(hb_glyph_info_get_glyph_flags(&info[n])&HB_GLYPH_FLAG_UNSAFE_TO_BREAK)!=0;
                if(part.rotate){item.x=pos[n].y_offset*factor;item.y=pos[n].x_offset*factor;item.advance=pos[n].x_advance*factor;}
                else {item.x=pos[n].x_offset*factor;item.y=-pos[n].y_offset*factor;item.advance=(vertical ? -pos[n].y_advance:pos[n].x_advance)*factor;}
                result.push_back(item);
            }
        }
    }
    return result;
}
std::shared_ptr<const LayoutResult> MakeLayout(detail::Face& face,detail::UnicodeText text,double size,const Style& style,const Limits& limits) {
    const bool vertical=style.writingMode!=WritingMode::Horizontal;
    const auto metrics=GetMetrics(face,size,vertical);
    const double pitch=metrics.pitch+style.lineSpacing;
    if(!std::isfinite(pitch) || pitch<=0)throw std::invalid_argument("Text default line pitch plus line spacing must be finite and positive");
    auto out=std::make_shared<LayoutResult>();out->font=face.font;out->style=style;out->size=size;out->text=text.text;out->defaultPitch=metrics.pitch;
    if(text.points.empty())return out;
    double maximumSpan{};
    for(std::size_t begin=0;;) {
        auto end=begin;while(end<text.points.size() && text.points[end]!='\n')++end;
        if(out->lines.size()>=limits.maxLines)throw std::length_error("Text line count limit exceeded");
        Line line;line.firstGlyph=out->glyphs.size();
        line.bounds=vertical ? Bounds{metrics.before,0,metrics.after,0}:Bounds{0,metrics.before,0,metrics.after};
        const auto shaped=Shape(face,text,begin,end,vertical,line.rightToLeft,limits.maxGlyphs-out->glyphs.size());
        if(shaped.size()>limits.maxGlyphs-out->glyphs.size())throw std::length_error("Text glyph count limit exceeded");
        double pen{};
        std::optional<std::uint32_t> previous;
        for(const auto& g:shaped) {
            if(g.cluster>=text.points.size())throw std::runtime_error("Text shaping returned invalid cluster");
            if(previous && *previous!=g.cluster) {
                const auto boundary=std::max(*previous,g.cluster);
                // HB unsafe-to-break means "reshape after a break", not
                // "prohibit spacing/breaking": ordinary kerning also sets it.
                // Preserve actual clusters and Unicode joining connections.
                if(text.grapheme[boundary] && !text.connection[boundary])pen+=style.letterSpacing;
            }
            const double center=g.clockwise ? -(face.face->ascender+face.face->descender)*size/(2*face.units):0;
            Glyph glyph{g.id,text.bytes[g.cluster],g.x*size+(vertical ? center:pen),g.y*size+(vertical ? pen:0),g.clockwise,text.connection[g.cluster]};
            if(auto ink=face.Ink(g.id,size,g.clockwise,style.outlineWidth)){Move(*ink,glyph.x,glyph.y);Union(line.bounds,*ink);}
            out->glyphs.push_back(glyph);
            if(vertical){line.bounds.top=std::min(line.bounds.top,pen);line.bounds.bottom=std::max(line.bounds.bottom,pen);}
            else {line.bounds.left=std::min(line.bounds.left,pen);line.bounds.right=std::max(line.bounds.right,pen);}
            pen+=g.advance*size;
            if(vertical){line.bounds.top=std::min(line.bounds.top,pen);line.bounds.bottom=std::max(line.bounds.bottom,pen);}
            else {line.bounds.left=std::min(line.bounds.left,pen);line.bounds.right=std::max(line.bounds.right,pen);}
            previous=g.cluster;
        }
        line.glyphCount=out->glyphs.size()-line.firstGlyph;ValidBounds(line.bounds);
        maximumSpan=std::max(maximumSpan,vertical ? line.bounds.Height():line.bounds.Width());out->lines.push_back(line);
        if(end==text.points.size())break;begin=end+1;
    }
    bool first=true;
    for(std::size_t i=0;i<out->lines.size();++i) {
        auto& line=out->lines[i];const double span=vertical ? line.bounds.Height():line.bounds.Width();
        const bool alignEnd=style.align==Align::Right || style.align==Align::Bottom ||
            (style.align==Align::End && (vertical || !line.rightToLeft)) || (style.align==Align::Start && !vertical && line.rightToLeft);
        const double extra=style.align==Align::Center ? (maximumSpan-span)/2:(alignEnd ? maximumSpan-span:0);
        double x{},y{};
        if(vertical){x=static_cast<double>(i)*pitch*(style.writingMode==WritingMode::VerticalRL ? -1:1);y=extra-line.bounds.top;}
        else {x=extra-line.bounds.left;y=static_cast<double>(i)*pitch;}
        Move(line.bounds,x,y);ValidBounds(line.bounds);
        for(auto g=line.firstGlyph;g<line.firstGlyph+line.glyphCount;++g){out->glyphs[g].x+=x;out->glyphs[g].y+=y;}
        if(first){out->bounds=line.bounds;first=false;}else Union(out->bounds,line.bounds);
    }
    ValidBounds(out->bounds);return out;
}
}
Anchor ParseAnchor(std::string_view s){return Parse<Anchor>(s,anchors);}
Align ParseAlign(std::string_view s){return Parse<Align>(s,aligns);}
WritingMode ParseWritingMode(std::string_view s){return Parse<WritingMode>(s,modes);}
std::string_view Name(Anchor x){return EnumName(x,anchors);}
std::string_view Name(Align x){return EnumName(x,aligns);}
std::string_view Name(WritingMode x){return EnumName(x,modes);}
Point LayoutResult::AnchorPoint() const noexcept {
    const auto index=static_cast<unsigned>(style.anchor);
    return {bounds.left+static_cast<double>(index%3)*bounds.Width()/2,bounds.top+static_cast<double>(index/3)*bounds.Height()/2};
}
struct Engine::LayoutCache {
    template<class T> struct OwnerLess {
        bool operator()(const std::shared_ptr<const T>& a,const std::shared_ptr<const T>& b) const noexcept {
            if(a.owner_before(b))return true;if(b.owner_before(a))return false;
            return std::less<const T*>{}(a.get(),b.get());
        }
    };
    struct Entry {
        std::size_t hash{},bytes{};
        std::string source;
        std::shared_ptr<const LayoutResult> layout;
    };
    struct Usage { std::size_t references{},bytes{}; };
    using Entries=std::list<Entry>;
    using Index=std::multimap<std::size_t,Entries::iterator>;
    using Fonts=std::map<std::shared_ptr<const Resource::Fonts::FontAsset>,Usage,OwnerLess<Resource::Fonts::FontAsset>>;
    using Buffers=std::map<std::shared_ptr<const std::string>,Usage,OwnerLess<std::string>>;
    // Account payload capacities, shared-object headers and conservative node
    // overhead. This bounds retained data, not the process allocator's exact RSS.
    static constexpr std::size_t sharedHeader=64,treeLinks=4*sizeof(void*),listLinks=2*sizeof(void*);
    const std::size_t maxBytes,maxEntries;
    std::size_t bytes{sizeof(LayoutCache)};
    Entries entries;
    Index index;
    Fonts fonts;
    Buffers buffers;
    std::mutex mutex;
    explicit LayoutCache(const Limits& limits):maxBytes(limits.maxLayoutCacheBytes),maxEntries(limits.maxLayoutCacheEntries){}
    static std::size_t Add(std::size_t a,std::size_t b) noexcept {
        return a>std::numeric_limits<std::size_t>::max()-b ? std::numeric_limits<std::size_t>::max():a+b;
    }
    static std::size_t FontBytes() noexcept {
        return sizeof(Resource::Fonts::FontAsset)+sharedHeader+sizeof(Fonts::value_type)+treeLinks;
    }
    static std::size_t BufferBytes(const std::string& value) noexcept {
        return Add(value.capacity(),sizeof(std::string)+1+sharedHeader+sizeof(Buffers::value_type)+treeLinks);
    }
    static std::size_t EntryBytes(const Entry& entry) noexcept {
        const auto& layout=*entry.layout;
        auto value=sizeof(Entry)+listLinks+sizeof(Index::value_type)+treeLinks+sizeof(LayoutResult)+sharedHeader;
        value=Add(value,Add(entry.source.capacity(),1));
        value=Add(value,Add(layout.text.capacity(),1));
        value=Add(value,layout.glyphs.capacity()*sizeof(Glyph));
        return Add(value,layout.lines.capacity()*sizeof(Line));
    }
    static bool SameOwner(const auto& a,const auto& b) noexcept {
        return a.get()==b.get() && !a.owner_before(b) && !b.owner_before(a);
    }
    static auto Bits(double value) noexcept { return std::bit_cast<std::uint64_t>(value); }
    static std::size_t Hash(const FontRef& font,std::string_view source,double size,const Style& s) noexcept {
        auto hash=std::hash<std::string_view>{}(source);
        const auto mix=[&](std::uint64_t value){hash^=std::hash<std::uint64_t>{}(value)+0x9e3779b97f4a7c15ull+(hash<<6)+(hash>>2);};
        mix(reinterpret_cast<std::uintptr_t>(font.asset.get()));mix(font.generation.owner);mix(font.generation.slot);mix(font.generation.generation);
        mix(Bits(size));mix(static_cast<unsigned>(s.anchor));mix(static_cast<unsigned>(s.align));mix(static_cast<unsigned>(s.writingMode));
        mix(Bits(s.letterSpacing));mix(Bits(s.lineSpacing));mix(Bits(s.outlineWidth));mix(s.outlineColor);return hash;
    }
    static bool Matches(const Entry& entry,const FontRef& font,std::string_view source,double size,const Style& s) noexcept {
        const auto& value=*entry.layout;const auto& prior=value.style;
        return value.font.generation==font.generation && SameOwner(value.font.asset,font.asset) && entry.source==source && Bits(value.size)==Bits(size) &&
            prior.anchor==s.anchor && prior.align==s.align && prior.writingMode==s.writingMode && Bits(prior.letterSpacing)==Bits(s.letterSpacing) &&
            Bits(prior.lineSpacing)==Bits(s.lineSpacing) && Bits(prior.outlineWidth)==Bits(s.outlineWidth) && prior.outlineColor==s.outlineColor;
    }
    std::shared_ptr<const LayoutResult> FindLocked(std::size_t hash,const FontRef& font,std::string_view source,double size,const Style& style) {
        const auto range=index.equal_range(hash);
        for(auto it=range.first;it!=range.second;++it)if(Matches(*it->second,font,source,size,style)) {
            entries.splice(entries.begin(),entries,it->second);return it->second->layout;
        }
        return {};
    }
    std::shared_ptr<const LayoutResult> Find(const FontRef& font,std::string_view source,double size,const Style& style) {
        std::lock_guard lock(mutex);return FindLocked(Hash(font,source,size,style),font,source,size,style);
    }
    void Evict() noexcept {
        auto entry=std::prev(entries.end());
        const auto range=index.equal_range(entry->hash);
        for(auto it=range.first;it!=range.second;++it)if(it->second==entry){index.erase(it);break;}
        bytes-=entry->bytes;
        const auto font=fonts.find(entry->layout->font.asset);
        if(--font->second.references==0) {
            const auto buffer=buffers.find(font->first->bytes);
            if(--buffer->second.references==0){bytes-=buffer->second.bytes;buffers.erase(buffer);}
            bytes-=font->second.bytes;fonts.erase(font);
        }
        entries.erase(entry);
    }
    std::shared_ptr<const LayoutResult> Remember(std::shared_ptr<const LayoutResult> layout,std::string_view source) {
        try {
            Entry entry{Hash(layout->font,source,layout->size,layout->style),0,std::string(source),layout};entry.bytes=EntryBytes(entry);
            // An oversized result must not evict useful content before bypassing.
            const auto minimum=Add(sizeof(LayoutCache),Add(entry.bytes,Add(FontBytes(),BufferBytes(*layout->font.asset->bytes))));
            if(minimum>maxBytes)return layout;
            std::lock_guard lock(mutex);
            if(auto found=FindLocked(entry.hash,layout->font,source,layout->size,layout->style))return found;
            for(;;) {
                const auto font=fonts.find(layout->font.asset);
                const auto buffer=buffers.find(layout->font.asset->bytes);
                auto needed=entry.bytes;
                if(font==fonts.end())needed=Add(needed,FontBytes());
                if(buffer==buffers.end())needed=Add(needed,BufferBytes(*layout->font.asset->bytes));
                if(entries.size()<maxEntries && needed<=maxBytes-bytes)break;
                if(entries.empty())return layout;Evict();
            }
            // Allocate every node before changing counts. Failed admission is
            // optional: roll back bookkeeping and still return the valid layout.
            auto buffer=buffers.end();auto font=fonts.end();auto added=entries.end();bool newBuffer{},newFont{};
            try {
                auto b=buffers.try_emplace(layout->font.asset->bytes,Usage{0,BufferBytes(*layout->font.asset->bytes)});buffer=b.first;newBuffer=b.second;
                auto f=fonts.try_emplace(layout->font.asset,Usage{0,FontBytes()});font=f.first;newFont=f.second;
                entries.push_front(std::move(entry));added=entries.begin();index.emplace(added->hash,added);
            } catch(...) {
                if(added!=entries.end())entries.erase(added);if(newFont)fonts.erase(font);if(newBuffer)buffers.erase(buffer);throw;
            }
            ++font->second.references;
            if(newFont){++buffer->second.references;bytes+=font->second.bytes;}
            if(newBuffer)bytes+=buffer->second.bytes;
            bytes+=added->bytes;
        } catch(const std::bad_alloc&) { /* Content caching must not fail a successful layout. */ }
        return layout;
    }
};

Engine::Engine(Limits value):limits(value) {
    if(!limits.maxTextBytes || !limits.maxGlyphs || !limits.maxLines || !limits.maxWrapCandidates || !limits.maxWrapShapeBytes)throw std::invalid_argument("Text CPU limits must be positive");
    if(limits.maxLayoutCacheBytes>=sizeof(LayoutCache) && limits.maxLayoutCacheEntries) {
        try {cache=std::make_unique<LayoutCache>(limits);} catch(const std::bad_alloc&) {}
    }
}
Engine::~Engine()=default;
std::shared_ptr<const LayoutResult> Engine::Layout(FontRef font,std::string_view text,double size,const Style& style) const {
    Validate(size,style);
    // A hit only reuses immutable, previously validated content. Invalid inputs
    // retain the original Decode-before-Face error order on the miss path.
    if(cache && text.size()<=limits.maxTextBytes && font.asset && font.asset->bytes &&
        !font.asset->bytes->empty() && font.asset->bytes->size()<=32*1024*1024) {
        if(auto found=cache->Find(font,text,size,style))return found;
    }
    auto decoded=detail::Decode(text,limits.maxTextBytes);detail::Face face(std::move(font));
    auto result=MakeLayout(face,std::move(decoded),size,style,limits);
    return cache ? cache->Remember(std::move(result),text):result;
}
std::optional<std::string> Engine::Wrap(FontRef font,std::string_view source,double size,const Style& style,double width,double height) const {
    Validate(size,style);Finite(width,"Text wrap width must be finite");Finite(height,"Text wrap height must be finite");
    if(width<=0 || height<=0)throw std::invalid_argument("Text wrap dimensions must be positive");
    auto text=detail::Decode(source,limits.maxTextBytes);detail::Face face(std::move(font));
    auto layout=MakeLayout(face,text,size,style,limits);
    if(layout->bounds.Width()<=width && layout->bounds.Height()<=height)return text.text;
    const bool vertical=style.writingMode!=WritingMode::Horizontal;const double limit=vertical ? height:width;
    std::set<std::uint32_t> clusterStarts,unsafe;
    for(const auto& glyph:layout->glyphs){clusterStarts.insert(glyph.cluster);if(glyph.unsafeBefore)unsafe.insert(glyph.cluster);}
    std::string output;std::size_t checked{},shapedBytes{};
    for(std::size_t first=0;;) {
        auto end=first;while(end<text.points.size() && text.points[end]!='\n')++end;
        auto start=first;
        while(start<end) {
            std::optional<std::size_t> preferred,emergency;
            // Negative spacing and contextual substitutions can make candidate
            // widths nonmonotone. Examine all safe ends, never stop at overflow.
            for(auto candidate=start+1;candidate<=end;++candidate) {
                const auto byte=text.bytes[candidate];
                const bool safe=candidate==end || (text.grapheme[candidate] && clusterStarts.contains(byte) && !unsafe.contains(byte));
                const bool normal=safe && (candidate==end || text.lineBreak[candidate]);
                if(!normal && !(safe && text.emergency[candidate]))continue;
                if(++checked>limits.maxWrapCandidates)throw std::length_error("Text wrap candidate work limit exceeded");
                const auto part=std::string_view(text.text).substr(text.bytes[start],byte-text.bytes[start]);
                if(part.size()>limits.maxWrapShapeBytes-shapedBytes)throw std::length_error("Text wrap shaping work byte limit exceeded");
                shapedBytes+=part.size();
                const auto trial=MakeLayout(face,detail::Decode(part,limits.maxTextBytes),size,style,limits);
                if((vertical ? trial->bounds.Height():trial->bounds.Width())<=limit) {
                    if(normal)preferred=candidate;else emergency=candidate;
                }
            }
            const auto chosen=preferred ? preferred:emergency;
            if(!chosen)return std::nullopt;
            output.append(text.text,text.bytes[start],text.bytes[*chosen]-text.bytes[start]);
            start=*chosen;if(start<end)output.push_back('\n');
        }
        if(end==text.points.size())break;output.push_back('\n');first=end+1;
        if(output.size()>limits.maxTextBytes)throw std::length_error("Text wrapped byte limit exceeded");
    }
    auto final=MakeLayout(face,detail::Decode(output,limits.maxTextBytes),size,style,limits);
    if(final->bounds.Width()>width || final->bounds.Height()>height)return std::nullopt;
    return output;
}

struct GlyphCache::Impl {
    struct Budget{std::atomic<std::size_t> bytes{};};
    struct Reservation{std::shared_ptr<Budget> owner;std::size_t bytes;~Reservation(){owner->bytes.fetch_sub(bytes);}};
    using Key=std::tuple<std::uint64_t,std::uint32_t,std::uint64_t,std::uint32_t,std::uint64_t,std::uint64_t,std::uint64_t,bool>;
    struct Entry{std::shared_ptr<const GlyphBitmap> bitmap;FontRef font;std::uint64_t age;};
    Limits limits;std::shared_ptr<Budget> budget{std::make_shared<Budget>()};std::map<Key,Entry> entries;std::uint64_t clock{};mutable std::mutex mutex;
    explicit Impl(Limits value):limits(value) {
        if(!limits.maxGlyphCacheBytes || !limits.maxGlyphBitmapBytes || !limits.maxGlyphDimension || !limits.maxGlyphCacheEntries)throw std::invalid_argument("Text glyph limits must be positive");
    }
    bool Evict(){
        auto oldest=entries.end();
        for(auto it=entries.begin();it!=entries.end();++it)if(it->second.bitmap.use_count()==1 && (oldest==entries.end() || it->second.age<oldest->second.age))oldest=it;
        if(oldest==entries.end())return false;entries.erase(oldest);return true;
    }
};
GlyphCache::GlyphCache(Limits value):impl(std::make_unique<Impl>(value)){}
GlyphCache::~GlyphCache()=default;
std::shared_ptr<const PreparedText> GlyphCache::Prepare(std::shared_ptr<const LayoutResult> layout,double scale) {
    if(!layout)throw std::invalid_argument("Text glyph preparation requires a layout");
    if(!std::isfinite(scale) || scale<=0)throw std::invalid_argument("Text raster scale must be finite and positive");
    auto result=std::make_shared<PreparedText>();result->layout=std::move(layout);result->glyphs.reserve(result->layout->glyphs.size());
    std::lock_guard lock(impl->mutex);std::unique_ptr<detail::Face> face;
    for(const auto& glyph:result->layout->glyphs) {
        const auto& l=*result->layout;const auto& h=l.font.generation;
        const Impl::Key key{h.owner,h.slot,h.generation,glyph.id,std::bit_cast<std::uint64_t>(l.size),std::bit_cast<std::uint64_t>(l.style.outlineWidth),std::bit_cast<std::uint64_t>(scale),glyph.clockwise};
        auto found=impl->entries.find(key);
        if(found==impl->entries.end()) {
            if(!face)face=std::make_unique<detail::Face>(l.font);
            auto bitmap=face->Raster(glyph.id,l.size,glyph.clockwise,l.style.outlineWidth,scale,impl->limits);
            const auto bytes=bitmap->fill.capacity()+bitmap->ring.capacity()+sizeof(GlyphBitmap);
            while((bytes>impl->limits.maxGlyphCacheBytes || impl->budget->bytes.load()>impl->limits.maxGlyphCacheBytes-bytes || impl->entries.size()>=impl->limits.maxGlyphCacheEntries) && impl->Evict()){}
            if(bytes>impl->limits.maxGlyphCacheBytes || impl->budget->bytes.load()>impl->limits.maxGlyphCacheBytes-bytes || impl->entries.size()>=impl->limits.maxGlyphCacheEntries)
                throw std::length_error("Text glyph cache is full with retained draw masks");
            auto reservation=std::make_shared<Impl::Reservation>();reservation->owner=impl->budget;reservation->bytes=bytes;
            impl->budget->bytes.fetch_add(bytes);
            std::const_pointer_cast<GlyphBitmap>(bitmap)->allocation=std::move(reservation);
            found=impl->entries.emplace(key,Impl::Entry{std::move(bitmap),l.font,++impl->clock}).first;
        } else found->second.age=++impl->clock;
        result->glyphs.push_back({found->second.bitmap,glyph.x,glyph.y});
    }
    return result;
}
void GlyphCache::Clear(){std::lock_guard lock(impl->mutex);impl->entries.clear();}
std::size_t GlyphCache::Bytes() const{return impl->budget->bytes.load();}
std::string_view Versions() noexcept{return "FreeType 2.14.1; HarfBuzz 12.3.0; SheenBidi 3.0.0; libunibreak 8.0; Unicode 17.0.0";}
}
