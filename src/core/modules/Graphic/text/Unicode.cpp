#include "Unicode.h"
#include <SheenBidi/SheenBidi.h>
#include <graphemebreak.h>
#include <linebreak.h>
#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>

namespace core::modules::Graphic::Text::detail {
namespace {
void Append(std::string& out, std::uint32_t cp) {
    if(cp<0x80) out.push_back(static_cast<char>(cp));
    else if(cp<0x800) { out.push_back(static_cast<char>(0xc0|(cp>>6))); out.push_back(static_cast<char>(0x80|(cp&63))); }
    else if(cp<0x10000) { out.push_back(static_cast<char>(0xe0|(cp>>12))); out.push_back(static_cast<char>(0x80|((cp>>6)&63))); out.push_back(static_cast<char>(0x80|(cp&63))); }
    else { out.push_back(static_cast<char>(0xf0|(cp>>18))); out.push_back(static_cast<char>(0x80|((cp>>12)&63))); out.push_back(static_cast<char>(0x80|((cp>>6)&63))); out.push_back(static_cast<char>(0x80|(cp&63))); }
}
std::uint32_t Next(std::string_view text, std::size_t& at) {
    const auto lead=static_cast<unsigned char>(text[at++]);
    if(lead<0x80) return lead;
    unsigned count{}; std::uint32_t cp{}, minimum{};
    if(lead>=0xc2 && lead<=0xdf) { count=1; cp=lead&31; minimum=0x80; }
    else if(lead>=0xe0 && lead<=0xef) { count=2; cp=lead&15; minimum=0x800; }
    else if(lead>=0xf0 && lead<=0xf4) { count=3; cp=lead&7; minimum=0x10000; }
    else throw std::invalid_argument("Text contains invalid UTF-8");
    if(count>text.size()-at) throw std::invalid_argument("Text contains truncated UTF-8");
    while(count--) { const auto c=static_cast<unsigned char>(text[at++]); if((c&0xc0)!=0x80) throw std::invalid_argument("Text contains invalid UTF-8 continuation"); cp=(cp<<6)|(c&63); }
    if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) throw std::invalid_argument("Text contains invalid Unicode scalar value");
    return cp;
}
bool Glue(std::uint32_t cp) { return cp==0xa0 || cp==0x202f || cp==0x2060 || cp==0xfeff; }
struct Algorithm { SBAlgorithmRef ref{}; ~Algorithm(){if(ref)SBAlgorithmRelease(ref);} };
struct Paragraph { SBParagraphRef ref{}; ~Paragraph(){if(ref)SBParagraphRelease(ref);} };
struct BidiLine { SBLineRef ref{}; ~BidiLine(){if(ref)SBLineRelease(ref);} };
struct Scripts { SBScriptLocatorRef ref{}; ~Scripts(){if(ref)SBScriptLocatorRelease(ref);} };
struct VerticalRange { std::uint32_t first,last; int value; };
#include "VerticalData.inc"
struct JoiningRange { std::uint32_t first,last; char value; };
#include "JoiningData.inc"
char Joining(std::uint32_t cp) {
    const auto it=std::lower_bound(std::begin(joiningRanges),std::end(joiningRanges),cp,
        [](const JoiningRange& r,std::uint32_t c){return r.last<c;});
    return it!=std::end(joiningRanges) && cp>=it->first ? it->value:'U';
}
}
UnicodeText Decode(std::string_view source, std::size_t maxBytes) {
    if(source.size()>maxBytes || source.size()>std::numeric_limits<std::uint32_t>::max()) throw std::length_error("Text byte limit exceeded");
    UnicodeText out;
    for(std::size_t i=0;i<source.size();) {
        auto cp=Next(source,i);
        if(cp=='\r') { if(i<source.size() && source[i]=='\n') ++i; cp='\n'; }
        const auto byte=static_cast<std::uint32_t>(out.text.size());
        Append(out.text,cp);
        for(int n=0;n<(cp=='\t' ? 4:1);++n) { out.points.push_back(cp=='\t' ? ' ':cp); out.bytes.push_back(byte); }
    }
    out.bytes.push_back(static_cast<std::uint32_t>(out.text.size()));
    const auto size=out.points.size();
    out.grapheme.assign(size+1,true); out.lineBreak.assign(size+1,true); out.emergency.assign(size+1,true);
    out.connection.assign(size+1,false);
    if(size) {
        std::vector<char> gr(size),lb(size);
        set_graphemebreaks_utf32(out.points.data(),size,"und",gr.data());
        set_linebreaks_utf32(out.points.data(),size,"und",lb.data());
        for(std::size_t i=1;i<size;++i) {
            out.grapheme[i]=gr[i-1]==GRAPHEMEBREAK_BREAK;
            const bool sourceBoundary=out.bytes[i]!=out.bytes[i-1];
            out.lineBreak[i]=(lb[i-1]==LINEBREAK_ALLOWBREAK || lb[i-1]==LINEBREAK_MUSTBREAK) && out.grapheme[i] && sourceBoundary;
            out.emergency[i]=out.grapheme[i] && sourceBoundary && !Glue(out.points[i-1]) && !Glue(out.points[i]);
        }
        std::optional<std::size_t> previous;char previousType='U';
        for(std::size_t i=0;i<size;++i) {
            const auto type=Joining(out.points[i]);
            if(type=='T')continue;
            if(previous && (previousType=='D'||previousType=='L'||previousType=='C') && (type=='D'||type=='R'||type=='C'))
                for(auto boundary=*previous+1;boundary<=i;++boundary)out.connection[boundary]=true;
            previous=i;previousType=type;
        }
    }
    return out;
}
int VerticalOrientation(std::uint32_t cp) {
    const auto it=std::lower_bound(std::begin(verticalRanges),std::end(verticalRanges),cp,
        [](const VerticalRange& r,std::uint32_t c){return r.last<c;});
    return it!=std::end(verticalRanges) && cp>=it->first ? it->value:1;
}
UnicodeRuns Analyze(const std::vector<std::uint32_t>& points,std::size_t begin,std::size_t end) {
    UnicodeRuns result;
    if(begin==end) return result;
    const SBCodepointSequence sequence{SBStringEncodingUTF32,points.data()+begin,end-begin};
    Algorithm algorithm{SBAlgorithmCreate(&sequence)};
    if(!algorithm.ref) throw std::runtime_error("Text bidi allocation failed");
    std::vector<std::uint32_t> scripts(end-begin);
    Scripts locator{SBScriptLocatorCreate()};
    if(!locator.ref) throw std::runtime_error("Text script allocation failed");
    SBScriptLocatorLoadCodepoints(locator.ref,&sequence);
    while(SBScriptLocatorMoveNext(locator.ref)) {
        const auto* a=SBScriptLocatorGetAgent(locator.ref);
        std::fill(scripts.begin()+a->offset,scripts.begin()+a->offset+a->length,SBScriptGetUnicodeTag(a->script));
    }
    // UBA paragraph separators besides LF are handled by the same Unicode
    // implementation; their zero-width controls remain in the source string.
    for(std::size_t offset=0;offset<end-begin;) {
        Paragraph paragraph{SBAlgorithmCreateParagraph(algorithm.ref,offset,end-begin-offset,SBLevelDefaultLTR)};
        if(!paragraph.ref) throw std::runtime_error("Text bidi paragraph failed");
        const auto length=SBParagraphGetLength(paragraph.ref);
        if(!length) throw std::runtime_error("Text bidi paragraph made no progress");
        if(offset==0) result.rtl=(SBParagraphGetBaseLevel(paragraph.ref)&1)!=0;
        BidiLine line{SBParagraphCreateLine(paragraph.ref,offset,length)};
        if(!line.ref) throw std::runtime_error("Text bidi line failed");
        const auto* runs=SBLineGetRunsPtr(line.ref);
        for(SBUInteger n=0;n<SBLineGetRunCount(line.ref);++n) {
            const auto& r=runs[n];
            std::vector<UnicodeRun> fragments;
            for(std::size_t i=r.offset;i<r.offset+r.length;) {
                auto last=i+1; while(last<r.offset+r.length && scripts[last]==scripts[i]) ++last;
                fragments.push_back({begin+i,begin+last,scripts[i],(r.level&1)!=0}); i=last;
            }
            if(r.level&1) std::reverse(fragments.begin(),fragments.end());
            result.runs.insert(result.runs.end(),fragments.begin(),fragments.end());
        }
        offset+=length;
    }
    return result;
}
}
