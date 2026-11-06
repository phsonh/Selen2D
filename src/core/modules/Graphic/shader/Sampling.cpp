#include "Sampling.h"
#include "DxcRuntime.h"
#include "ImageSamplingSource.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <dxc/dxcapi.h>
#include <dxc/dxcisense.h>
#include <wrl/client.h>
#endif

namespace core::modules::Graphic::Shader::Sampling {
std::string Function(std::string_view texture,bool explicitLod) {
    return std::string(explicitLod?"EngineSampleLevel_":"EngineSample_")+std::string(texture);
}
std::string Prototypes(const Model& model) {
    std::string result="\n#line 1 \"Engine.generated.hlsl\"\n";
    for(const auto& d:model.declarations) if(d.kind==DeclarationKind::Texture && d.textureRole==TextureRole::Color) {
        result+="float4 "+Function(d.name,false)+"(SamplerState,float2);\n";
        result+="float4 "+Function(d.name,true)+"(SamplerState,float2,float);\n";
    }
    return result;
}
std::string Definitions(const Model& model) {
    std::string result="\n#line 1 \"Engine.generated.hlsl\"\n";
    result+=Source;
    for(const auto& d:model.declarations) if(d.kind==DeclarationKind::Texture && d.textureRole==TextureRole::Color) {
        const auto region="EngineRegion_"+d.name,info="EngineInfo_"+d.name;
        const std::string flags="float4(1,8+"+info+".w,"+info+".z,0)";
        result+="float4 "+Function(d.name,false)+"(SamplerState EngineSampler,float2 EngineUv) {\n";
        result+=" return EngineUnassociate(EngineSampleImageGrad("+d.name+",EngineSampler,EngineUv,ddx(EngineUv),ddy(EngineUv),float4(0,0,1,1),"+region+","+flags+"));\n}\n";
        result+="float4 "+Function(d.name,true)+"(SamplerState EngineSampler,float2 EngineUv,float EngineLod) {\n";
        result+=" return EngineUnassociate(EngineSampleImageLod("+d.name+",EngineSampler,EngineUv,EngineLod,float4(0,0,1,1),"+region+","+flags+"));\n}\n";
    }
    return result;
}
#ifdef _WIN32
namespace {
using Microsoft::WRL::ComPtr;
void HR(HRESULT result) {
    if(FAILED(result)) throw std::runtime_error("Shader semantic analysis failed (HRESULT "+std::to_string(result)+")");
}
void Cancel(const std::function<bool()>& cancelled) {
    if(cancelled && cancelled()) throw std::runtime_error("Shader compilation cancelled");
}
struct Text {
    LPSTR value{};
    ~Text(){CoTaskMemFree(value);}
    std::string String() const {return value?value:"";}
};
struct WideText {BSTR value{};~WideText(){SysFreeString(value);}};
struct Children {
    IDxcCursor** data{};unsigned size{};
    ~Children(){for(unsigned i=0;i<size;++i) data[i]->Release();CoTaskMemFree(data);}
};
struct Range {unsigned begin{},end{}; auto operator<=>(const Range&) const = default;};
Range Extent(IDxcCursor* cursor) {
    ComPtr<IDxcSourceRange> source;HR(cursor->GetExtent(&source));Range result;
    HR(source->GetOffsets(&result.begin,&result.end));return result;
}
DxcCursorKind Kind(IDxcCursor* cursor) {DxcCursorKind value{};HR(cursor->GetKind(&value));return value;}
std::string Name(IDxcCursor* cursor) {Text text;HR(cursor->GetSpelling(&text.value));return text.String();}
unsigned Offset(IDxcCursor* cursor) {
    ComPtr<IDxcSourceLocation> source;HR(cursor->GetLocation(&source));unsigned result{};
    HR(source->GetSpellingLocation(nullptr,nullptr,nullptr,&result));return result;
}
Location Where(IDxcCursor* cursor) {
    ComPtr<IDxcSourceLocation> source;HR(cursor->GetLocation(&source));Text path;unsigned line{},column{};
    HR(source->GetPresumedLocation(&path.value,&line,&column));return {path.String(),line,column};
}
[[noreturn]] void Error(IDxcCursor* cursor,std::string_view reason) {
    throw std::invalid_argument(Describe(Where(cursor))+": "+std::string(reason));
}
ComPtr<IDxcCursor> Reference(IDxcCursor* cursor,unsigned depth=0) {
    if(depth>64) Error(cursor,"Shader reference expression nesting limit exceeded");
    if(Kind(cursor)==DxcCursor_DeclRefExpr) return cursor;
    // Implicit casts and parentheses have one expression child. Do not infer
    // a resource identity from arbitrary expressions or user helper results.
    if(Kind(cursor)!=DxcCursor_UnexposedExpr && Kind(cursor)!=DxcCursor_ParenExpr) return {};
    Children children;HR(cursor->GetChildren(0,2,&children.size,&children.data));
    return children.size==1 ? Reference(children.data[0],depth+1) : ComPtr<IDxcCursor>{};
}
const Declaration* DeclarationOf(IDxcCursor* reference,const std::map<unsigned,const Declaration*>& globals) {
    ComPtr<IDxcCursor> declaration;HR(reference->GetReferencedCursor(&declaration));
    if(!declaration) return nullptr;
    const auto found=globals.find(Offset(declaration.Get()));
    return found==globals.end()?nullptr:found->second;
}
struct Analysis {
    const Model& model;
    const std::function<bool()>& cancelled;
    std::map<unsigned,const Declaration*> globals;
    std::set<Range> permitted;
    std::vector<ComPtr<IDxcCursor>> references;
    std::vector<SamplingCall> calls;
    std::size_t nodes{};
    Analysis(const Model& m,const std::function<bool()>& c):model(m),cancelled(c) {
        for(const auto& d:model.declarations) if(d.kind==DeclarationKind::Texture || d.kind==DeclarationKind::Sampler)
            globals.emplace(static_cast<unsigned>(d.bindingOffset-d.name.size()),&d);
    }
    void Call(IDxcCursor* cursor) {
        Children children;HR(cursor->GetChildren(0,1,&children.size,&children.data));
        if(children.size!=1 || Kind(children.data[0])!=DxcCursor_MemberRefExpr) return;
        auto* member=children.data[0];
        Children objects;HR(member->GetChildren(0,2,&objects.size,&objects.data));
        if(objects.size!=1) return;
        auto reference=Reference(objects.data[0]);if(!reference) return;
        const auto* declaration=DeclarationOf(reference.Get(),globals);
        if(!declaration || declaration->kind!=DeclarationKind::Texture) return;
        const auto method=Name(member);
        int count{};HR(cursor->GetNumArguments(&count));
        const bool color=declaration->textureRole==TextureRole::Color;
        if(color && !((method=="Sample" && count==2)||(method=="SampleLevel" && count==3)))
            Error(cursor,"color Texture2D supports only Sample(pairedSampler, uv) and SampleLevel(pairedSampler, uv, lod)");
        if(!color) return; // Numeric reads retain their native HLSL semantics.
        if(method.starts_with("Sample") || method.starts_with("Gather")) {
            if(count<1) Error(cursor,"Texture2D sampling requires its paired sampler");
            ComPtr<IDxcCursor> argument;HR(cursor->GetArgumentAt(0,&argument));
            const auto samplerReference=Reference(argument.Get());
            const auto* sampler=samplerReference?DeclarationOf(samplerReference.Get(),globals):nullptr;
            if(!sampler || sampler->kind!=DeclarationKind::Sampler || sampler->name!=declaration->name+"Sampler")
                Error(cursor,"Texture2D sampling must use its matching global sampler: "+declaration->name+"Sampler");
        }
        const auto extent=Extent(member);
        if(extent.begin>=extent.end || extent.end>model.source.size()) Error(cursor,"invalid semantic sampling source range");
        permitted.insert(Extent(reference.Get()));
        calls.push_back({extent.begin,extent.end,declaration->name,method=="SampleLevel"});
    }
    void Visit(IDxcCursor* cursor,unsigned depth=0) {
        if(depth>1024 || ++nodes>262144) Error(cursor,"Shader semantic tree exceeds bounded analysis limits");
        if((nodes&1023)==0) Cancel(cancelled);
        const auto kind=Kind(cursor);
        if(kind==DxcCursor_CallExpr) Call(cursor);
        if(kind==DxcCursor_DeclRefExpr) {
            const auto* d=DeclarationOf(cursor,globals);
            if(d && d->kind==DeclarationKind::Texture && d->textureRole==TextureRole::Color) references.emplace_back(cursor);
        }
        // Reserved identifiers include local declarations and struct fields,
        // not merely the top-level declaration subset handled by Model.
        if(kind>=DxcCursor_UnexposedDecl && kind<=DxcCursor_LastDecl) {
            const auto name=Name(cursor);
            const auto where=Where(cursor);
            if(name.starts_with("Engine") && where.file!="Engine.hlsl") Error(cursor,"the Engine identifier prefix is reserved");
        }
        for(unsigned skip=0;;) {
            Children children;HR(cursor->GetChildren(skip,64,&children.size,&children.data));
            for(unsigned i=0;i<children.size;++i) Visit(children.data[i],depth+1);
            skip+=children.size;if(children.size<64) break;
        }
    }
};
}
#endif
std::vector<SamplingCall> Analyze(const Model& model,const std::function<bool()>& cancelled) {
#ifdef _WIN32
    Cancel(cancelled);
    if(model.source.size()>std::numeric_limits<unsigned>::max()) throw std::length_error("Shader semantic source is too large");
    ComPtr<IDxcIntelliSense> sense;HR(DxcRuntime::Create(CLSID_DxcIntelliSense,IID_PPV_ARGS(&sense)));
    ComPtr<IDxcIndex> index;HR(sense->CreateIndex(&index));
    ComPtr<IDxcUnsavedFile> unsaved;
    constexpr const char* filename="Engine.semantic.hlsl";
    HR(sense->CreateUnsavedFile(filename,model.source.data(),static_cast<unsigned>(model.source.size()),&unsaved));
    auto* input=unsaved.Get();ComPtr<IDxcTranslationUnit> unit;
    const char* args[]{"-HV","2021"};
    HR(index->ParseTranslationUnit(filename,args,2,&input,1,DxcTranslationUnitFlags_UseCallerThread,&unit));
    Cancel(cancelled);
    unsigned count{};HR(unit->GetNumDiagnostics(&count));
    for(unsigned i=0;i<count;++i) {
        ComPtr<IDxcDiagnostic> diagnostic;HR(unit->GetDiagnostic(i,&diagnostic));DxcDiagnosticSeverity severity{};
        HR(diagnostic->GetSeverity(&severity));
        if(severity<DxcDiagnostic_Error) continue;
        // This fixed DXC's IDxcDiagnostic::GetSpelling/GetLocation omit its
        // thread allocator scope. Use the TU accessor (which owns the scope),
        // then obtain a fresh TU location for the original #line mapping.
        Text message,path;HR(diagnostic->FormatDiagnostic(DxcDiagnostic_DisplaySeverity,&message.value));
        unsigned code{},line{},column{},offset{},length{};WideText diagnosticFile,diagnosticText;
        HR(unit->GetDiagnosticDetails(i,DxcDiagnostic_DisplaySeverity,&code,&line,&column,
            &diagnosticFile.value,&offset,&length,&diagnosticText.value));
        ComPtr<IDxcFile> file;HR(unit->GetFile(filename,&file));
        ComPtr<IDxcSourceLocation> source;HR(unit->GetLocationForOffset(file.Get(),offset,&source));
        HR(source->GetPresumedLocation(&path.value,&line,&column));
        throw std::invalid_argument(Describe({path.String(),line,column})+": Shader semantic analysis: "+message.String().substr(0,65536));
    }
    ComPtr<IDxcCursor> root;HR(unit->GetCursor(&root));Analysis analysis(model,cancelled);analysis.Visit(root.Get());
    for(const auto& reference:analysis.references) if(!analysis.permitted.contains(Extent(reference.Get())))
        Error(reference.Get(),"color Texture2D must be sampled directly; aliases, resource arguments and other access forms are not supported");
    std::sort(analysis.calls.begin(),analysis.calls.end(),[](const auto& a,const auto& b){return a.begin<b.begin;});
    return std::move(analysis.calls);
#else
    (void)model;(void)cancelled;throw std::runtime_error("Shader semantic analysis is unavailable on this platform");
#endif
}
}
