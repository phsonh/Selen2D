#include "Compiler.h"
#include "Sampling.h"
#include "core/modules/File/Vfs.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <dxc/dxcapi.h>
#include <EngineDxcStatic.h>
#include <d3d12shader.h>
#include <wrl/client.h>
#endif

namespace core::modules::Graphic::Shader {
std::size_t Compiled::ResidentBytes() const noexcept {
    std::size_t result=sizeof(*this)+path.capacity()+model.source.capacity();
    result+=model.declarations.capacity()*sizeof(Declaration)+model.parameters.capacity()*sizeof(Parameter)+model.sampling.capacity()*sizeof(SamplingCall);
    for(const auto& d:model.declarations) result+=d.name.capacity()+d.location.file.capacity()+d.mainTexture.capacity();
    for(const auto& p:model.parameters) result+=p.name.capacity()+p.location.file.capacity();
    for(const auto& c:model.sampling) result+=c.texture.capacity();
    for(const auto* s:{&vertex,&fragment}) {
        result+=s->code.capacity()+s->entry.capacity()+s->textures.capacity()*sizeof(TextureBinding)+s->buffers.capacity()*sizeof(BufferBinding);
        for(const auto& t:s->textures) result+=t.name.capacity();
        for(const auto& b:s->buffers) {
            result+=b.name.capacity()+b.fields.capacity()*sizeof(Field);
            for(const auto& f:b.fields) result+=f.name.capacity();
        }
    }
    return result;
}
namespace {
constexpr std::size_t MiB=1024*1024;
constexpr std::size_t SourceLimit=8*MiB,PreprocessLimit=16*MiB,CodeLimit=16*MiB,DiagnosticLimit=64*1024;
void CheckCancelled(const std::function<bool()>& cancelled) {
    if(cancelled && cancelled()) throw std::runtime_error("Shader compilation cancelled");
}

// User #line would otherwise forge the trusted builtin source identity after
// preprocessing. Normalize escaped newlines, then inspect directives outside
// comments/string literals. No HLSL code is rewritten by this validation.
void ValidateDirectives(std::string_view input,const std::string& path) {
    if(input.find('\0')!=std::string_view::npos) throw std::invalid_argument(path+": embedded NUL in HLSL");
    if(input.starts_with("\xEF\xBB\xBF")) input.remove_prefix(3);
    std::string text; text.reserve(input.size());
    for(std::size_t i=0;i<input.size();++i) {
        if(input[i]=='\\' && i+1<input.size()) {
            if(input[i+1]=='\n') { ++i; continue; }
            if(input[i+1]=='\r') {
                ++i;
                if(i+1<input.size() && input[i+1]=='\n') ++i;
                continue;
            }
        }
        if(input[i]=='\r') {
            if(i+1<input.size() && input[i+1]=='\n') ++i;
            text+='\n';
        } else text+=input[i];
    }
    bool block=false,quoted=false,lineComment=false,escaped=false,lineStart=true;
    unsigned line=1;
    for(std::size_t i=0;i<text.size();++i) {
        const char c=text[i],n=i+1<text.size()?text[i+1]:'\0';
        if(c=='\n') { ++line; lineStart=true; lineComment=false; escaped=false; continue; }
        if(lineComment) continue;
        if(block) { if(c=='*' && n=='/') { block=false; ++i; } continue; }
        if(quoted) { if(!escaped && c=='"') quoted=false; escaped=!escaped && c=='\\'; continue; }
        if(c=='/' && n=='*') { block=true; ++i; continue; }
        if(c=='/' && n=='/') { lineComment=true; ++i; continue; }
        if(c=='"') { quoted=true; lineStart=false; continue; }
        if(c==' ' || c=='\t' || c=='\r' || c=='\f' || c=='\v') continue;
        if((c=='#' || (c=='%' && n==':')) && lineStart) {
            const auto trivia=[&](std::size_t& at) {
                for(;;) {
                    while(at<text.size() && (text[at]==' ' || text[at]=='\t' || text[at]=='\r' || text[at]=='\f' || text[at]=='\v')) ++at;
                    if(at+1>=text.size() || text[at]!='/' || text[at+1]!='*') return;
                    const auto close=text.find("*/",at+2);
                    if(close==std::string::npos) { at=text.size(); return; }
                    at=close+2;
                }
            };
            std::size_t start=i+(c=='#'?1:2);
            trivia(start);
            std::size_t end=start;
            while(end<text.size() && ((text[end]>='a'&&text[end]<='z') || (text[end]>='A'&&text[end]<='Z') ||
                (text[end]>='0'&&text[end]<='9') || text[end]=='_')) ++end;
            const auto directive=std::string_view(text).substr(start,end-start);
            if(directive=="line" || (!directive.empty() && directive.front()>='0' && directive.front()<='9'))
                throw std::invalid_argument(path+":"+std::to_string(line)+":1: user #line is unsupported (engine source identities are reserved)");
            if(directive=="include") {
                trivia(end);
                if(end<text.size() && (text[end]=='"' || text[end]=='<')) {
                    const auto close=text[end]=='"'?'"':'>';
                    const auto begin=++end;
                    while(end<text.size() && text[end]!=close && text[end]!='\n') ++end;
                    if(end==text.size() || text[end]!=close)
                        throw std::invalid_argument(path+":"+std::to_string(line)+":1: unterminated Shader include path");
                    try {if(!File::Normalize(std::string_view(text).substr(begin,end-begin))) throw std::invalid_argument("invalid path");}
                    catch(const std::exception&) {throw std::invalid_argument(path+":"+std::to_string(line)+":1: Shader include requires a strict relative VFS path without '.' or '..' components");}
                }
            }
            if(directive=="define" || directive=="undef") {
                trivia(end);
                const auto begin=end;
                while(end<text.size() && ((text[end]>='a'&&text[end]<='z') || (text[end]>='A'&&text[end]<='Z') ||
                    (text[end]>='0'&&text[end]<='9') || text[end]=='_')) ++end;
                const auto name=std::string_view(text).substr(begin,end-begin);
                if(name.starts_with("Engine") || name=="VSInput" || name=="PSInput" || name=="ENGINE_HLSL")
                    throw std::invalid_argument(path+":"+std::to_string(line)+":1: reserved Shader ABI macro");
            }
        }
        lineStart=false;
    }
}

constexpr const char* Builtin=R"HLSL(#line 1 "Engine.hlsl"
#ifndef ENGINE_HLSL
#define ENGINE_HLSL
struct VSInput {
    float3 position : POSITION0;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 worldPosition : TEXCOORD1;
};
cbuffer EngineBuiltins {
    column_major float4x4 EngineView;
    column_major float4x4 EngineProjection;
    column_major float4x4 EngineViewProjection;
    column_major float4x4 EngineCanvasToClip;
    float2 EngineTargetSize;
    float2 EngineTargetInvSize;
    uint EngineIsFullscreen;
};
#endif
)HLSL";
// The public VSInput ABI differs from SDL's fixed TEXCOORD vertex attributes.
// Keep that adaptation entirely inside the compiler boundary.
constexpr const char* Adapter=R"HLSL(
#line 1 "Engine.generated.hlsl"
struct EngineNativeInput {
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};
PSInput EngineVertexEntry(EngineNativeInput nativeInput) {
    VSInput input;
    input.position=nativeInput.position;
    input.uv=nativeInput.uv;
    input.color=nativeInput.color;
    return VSMain(input);
}
)HLSL";
constexpr const char* DefaultVertex=R"HLSL(
#line 1 "Engine.generated.hlsl"
PSInput VSMain(VSInput input) {
    PSInput result;
    float4 p=float4(input.position,1.0);
    result.position=EngineIsFullscreen!=0 ? p :
        (EngineTargetKind.x>0.5 ? mul(EngineViewProjection,p) : mul(EngineCanvasToClip,p));
    result.uv=input.uv;
    result.color=input.color;
    result.worldPosition=input.position;
    return result;
}
)HLSL";

// Only the generator emits these declarations, after parsing the complete user
// source. They never enter Model::declarations or its Lua parameter inventory.
// Fields are reflected and uploaded like user buffers, rather than relying on
// a hardcoded GPU register or assuming optimization preserves a whole block.
constexpr const char* DrawStateName="EngineDrawState";
constexpr const char* FogDistanceName="EngineFogDistance";
constexpr const char* FragmentFunctions=R"HLSL(
float EngineStableLength(float3 value) {
    float scale=max(abs(value.x),max(abs(value.y),abs(value.z)));
    if(scale==0) return 0;
    precise float3 normalized=value/scale;
    return scale*length(normalized);
}
float4 EngineFogBase(float4 baseColor,float cameraDistance) {
    float t=saturate((cameraDistance-EngineFogRange.x)/(EngineFogRange.y-EngineFogRange.x));
    int mode=(int)EngineFogRange.z;
    float v=mode>=4 ? 1-t : t;
    float factor=v;
    if(mode!=0) {
        factor=v*v;
        if(mode==2 || mode==3 || mode==5 || mode==6) factor*=v;
        if(mode==3 || mode==6) factor*=v;
    }
    if(mode>=4) factor=1-factor;
    int rgbMode=(int)EngineFogOps.x;
    if(rgbMode==0) baseColor.rgb=lerp(baseColor.rgb,EngineFogColor.rgb,factor);
    else if(rgbMode==1) baseColor.rgb+=EngineFogColor.rgb*factor;
    else if(rgbMode==2) baseColor.rgb-=EngineFogColor.rgb*factor;
    else if(rgbMode==3) baseColor.rgb*=1-factor+EngineFogColor.rgb*factor;
    else baseColor.rgb=lerp(baseColor.rgb,1-(1-baseColor.rgb)*(1-EngineFogColor.rgb),factor);
    baseColor.rgb=saturate(baseColor.rgb);
    int alphaMode=(int)EngineFogOps.y;
    if(alphaMode==1) baseColor.a*=1-factor+factor*EngineFogColor.a;
    else if(alphaMode==2) baseColor.a*=1-factor;
    return baseColor;
}
struct EngineFragmentOutput {
    float4 color : SV_Target0;
    float distance : SV_Target1;
};
)HLSL";

#ifdef _WIN32
using Microsoft::WRL::ComPtr;
void HR(HRESULT result,const char* reason) {
    if(FAILED(result)) throw std::runtime_error(std::string(reason)+" (HRESULT="+std::to_string(static_cast<unsigned long>(result))+")");
}
std::wstring Wide(std::string_view input) {
    if(input.size()>static_cast<std::size_t>(std::numeric_limits<int>::max())) throw std::length_error("Shader path too long");
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),nullptr,0);
    if(!count && !input.empty()) throw std::invalid_argument("invalid UTF-8 Shader path");
    std::wstring result(count,L'\0');
    if(count) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),result.data(),count);
    return result;
}
std::string Utf8(const wchar_t* input) {
    const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,input,-1,nullptr,0,nullptr,nullptr);
    if(count<=0) throw std::invalid_argument("invalid Shader include filename");
    std::string result(count,'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,input,-1,result.data(),count,nullptr,nullptr);
    result.pop_back(); return result;
}
struct Library {
    DxcCreateInstanceProc create{};
    Library() {
        // The compiler and validator are real static implementations. Keep their
        // process lifetime independent of repeated Engine Init/Shutdown cycles.
        HR(EngineDxcInitialize(),"initialize static Shader compiler");
        create=&DxcCreateInstance;
    }
    // Normal host shutdown has already drained resource jobs and COM owners.
    ~Library() { EngineDxcShutdown(); }
};
const Library& RuntimeCompiler() { static const Library value; return value; }

class Includes final : public IDxcIncludeHandler {
public:
    Includes(ComPtr<IDxcUtils> utilities,std::shared_ptr<const File::FileSystem> fs,std::function<bool()> stop)
        :utils(std::move(utilities)),files(std::move(fs)),cancelled(std::move(stop)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** result) override {
        if(!result) return E_POINTER;
        *result=nullptr;
        if(id==__uuidof(IUnknown) || id==__uuidof(IDxcIncludeHandler)) { *result=static_cast<IDxcIncludeHandler*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n=--references; if(!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR filename,IDxcBlob** output) override {
        if(!output) return E_POINTER;
        *output=nullptr;
        try {
            CheckCancelled(cancelled);
            auto requested=Utf8(filename);
            // DXC NormalizePathW converts '/' to '\\' and prefixes '.\\' on
            // Windows. Translate this backend spelling at the include boundary,
            // not in the public File/Resource path validator. VFS still rejects
            // parent traversal, absolute paths and unauthorized locations.
            std::replace(requested.begin(),requested.end(),'\\','/');
            if(requested.starts_with("./")) requested.erase(0,2);
            // Validate syntax before the virtual builtin exception as well.
            // Only the single backend-added prefix is removed; user traversal
            // or absolute paths never become a way to reach Engine.hlsl.
            if(!File::Normalize(requested)) throw std::invalid_argument("Shader include has invalid absolute path or dot component");
            const auto base=requested.find_last_of("/\\");
            if(requested.substr(base==std::string::npos?0:base+1)=="Engine.hlsl") {
                builtinIncluded=true;
                ComPtr<IDxcBlobEncoding> blob;
                HR(utils->CreateBlob(Builtin,static_cast<UINT32>(std::char_traits<char>::length(Builtin)),DXC_CP_UTF8,&blob),"create builtin include");
                *output=blob.Detach(); return S_OK;
            }
            const auto path=files->NormalizePath(requested);
            if(!path || path->empty()) throw std::invalid_argument("include path leaves VFS: "+requested);
            const auto& bytes=Read(*path);
            ComPtr<IDxcBlobEncoding> blob;
            HR(utils->CreateBlob(bytes.data(),static_cast<UINT32>(bytes.size()),DXC_CP_UTF8,&blob),"create Shader include");
            *output=blob.Detach(); return S_OK;
        } catch(const std::exception& error) {
            try { lastError=std::string(error.what()).substr(0,DiagnosticLimit); } catch(...) {}
            return E_FAIL;
        } catch(...) { return E_FAIL; }
    }
    const std::string& Read(const std::string& path) {
        if(const auto found=cache.find(path);found!=cache.end()) return found->second;
        if(cache.size()>=128) throw std::length_error("Shader include count exceeds 128");
        File::ReadOptions options;
        options.maxBytes=SourceLimit-total; options.prefixBytes=0; options.cancelled=cancelled;
        if(!options.maxBytes) throw std::length_error("Shader source limit exceeded");
        auto snapshot=files->ReadSnapshot(path,options);
        ValidateDirectives(snapshot.bytes,path);
        total+=snapshot.bytes.size();
        return cache.emplace(path,std::move(snapshot.bytes)).first->second;
    }
    std::string lastError;
    bool builtinIncluded{};
private:
    std::atomic<ULONG> references{1};
    ComPtr<IDxcUtils> utils;
    std::shared_ptr<const File::FileSystem> files;
    std::function<bool()> cancelled;
    std::map<std::string,std::string> cache;
    std::size_t total{};
};

struct Compiler {
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<Includes> includes;
    std::string path;
    std::function<bool()> cancelled;
    Compiler(std::shared_ptr<const File::FileSystem> files,std::string p,std::function<bool()> stop)
        :path(std::move(p)),cancelled(std::move(stop)) {
        const auto& library=RuntimeCompiler();
        HR(library.create(CLSID_DxcUtils,IID_PPV_ARGS(&utils)),"create DXC utilities");
        HR(library.create(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)),"create DXC compiler");
        includes.Attach(new Includes(utils,std::move(files),cancelled));
    }
    ComPtr<IDxcResult> Run(std::string_view source,const std::vector<std::wstring>& options,const char* entry) {
        CheckCancelled(cancelled);
        DxcBuffer buffer{source.data(),source.size(),DXC_CP_UTF8};
        std::vector<LPCWSTR> args; args.reserve(options.size());
        for(const auto& option:options) args.push_back(option.c_str());
        ComPtr<IDxcResult> result;
        HR(compiler->Compile(&buffer,args.data(),static_cast<UINT32>(args.size()),includes.Get(),IID_PPV_ARGS(&result)),"invoke DXC");
        CheckCancelled(cancelled);
        HRESULT status{}; HR(result->GetStatus(&status),"read DXC status");
        if(FAILED(status)) {
            ComPtr<IDxcBlobUtf8> errors;
            std::string reason=path+" ["+entry+"]: ";
            if(SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&errors),nullptr)) && errors)
                reason.append(errors->GetStringPointer(),std::min(errors->GetStringLength(),DiagnosticLimit));
            if(!includes->lastError.empty()) reason+="\n"+includes->lastError;
            if(!includes->builtinIncluded) reason+="\nPublic VSInput/PSInput require #include \"Engine.hlsl\".";
            throw std::runtime_error(reason);
        }
        return result;
    }
    std::string Preprocess() {
        const auto& input=includes->Read(path);
        const auto parent=path.substr(0,path.find_last_of('/')==std::string::npos?0:path.find_last_of('/'));
        const auto result=Run(input,{Wide("./"+path),L"-P",L"-HV",L"2021",L"-I",Wide(parent.empty()?".":parent)},"preprocess");
        ComPtr<IDxcBlobUtf8> output;
        HR(result->GetOutput(DXC_OUT_HLSL,IID_PPV_ARGS(&output),nullptr),"read preprocessed HLSL");
        if(!output || output->GetStringLength()>PreprocessLimit) throw std::length_error("preprocessed HLSL exceeds 16 MiB");
        return {output->GetStringPointer(),output->GetStringLength()};
    }
};

struct Assignment {
    char prefix{};
    std::uint32_t slot{},space{};
};
using Bindings=std::map<std::string,Assignment>;
struct Active { std::set<std::string> textures,buffers; };
Bindings Assign(const Model& model,bool vertex,const Active* active) {
    Bindings result;
    std::vector<std::string> textures,buffers;
    for(const auto& d:model.declarations) {
        if(d.kind==DeclarationKind::Texture) textures.push_back(d.name);
        if(d.kind==DeclarationKind::Builtins || d.kind==DeclarationKind::Params) buffers.push_back(d.name);
    }
    if(!vertex) textures.emplace_back(FogDistanceName);
    // Target kind is a private per-Draw value used by both the default VS and
    // fragment wrapper. It never enters the public/semantic Engine.hlsl input.
    buffers.emplace_back(DrawStateName);
    if(std::any_of(model.declarations.begin(),model.declarations.end(),[](const auto& d) {
        return d.kind==DeclarationKind::Texture && d.textureRole==TextureRole::Color;
    })) buffers.emplace_back(Sampling::BufferName);
    if(active) {
        std::stable_sort(textures.begin(),textures.end(),[&](const auto& a,const auto& b) { return active->textures.contains(a)>active->textures.contains(b); });
        std::stable_sort(buffers.begin(),buffers.end(),[&](const auto& a,const auto& b) { return active->buffers.contains(a)>active->buffers.contains(b); });
    }
    for(std::uint32_t i=0;i<textures.size();++i) {
        result.emplace(textures[i],Assignment{'t',i,vertex?0u:2u});
        result.emplace(textures[i]+"Sampler",Assignment{'s',i,vertex?0u:2u});
    }
    for(std::uint32_t i=0;i<buffers.size();++i) result.emplace(buffers[i],Assignment{'b',i,vertex?1u:3u});
    return result;
}
std::string Register(const Assignment& value) {
    return " : register("+std::string(1,value.prefix)+std::to_string(value.slot)+", space"+std::to_string(value.space)+")";
}
std::string DrawState(const Bindings& bindings) {
    std::string result="\n#line 1 \"Engine.generated.hlsl\"\ncbuffer ";
    result+=DrawStateName+Register(bindings.at(DrawStateName))+R"HLSL( {
    float4 EngineFogRange; // start, end, curve 0..6, enabled
    float4 EngineFogColor; // straight RGBA
    float4 EngineFogOps; // RGB enum, Alpha enum, reserved, reserved
    float4 EngineCameraPosition; // xyz world position, reserved
    float4 EngineTargetKind; // Canvas3D flag, reserved, reserved, reserved
};
)HLSL";
    return result;
}
std::string Fragment(const Model& model,const Bindings& bindings) {
    std::string result;
    result+="Texture2D<float> "+std::string(FogDistanceName)+Register(bindings.at(FogDistanceName))+";\n";
    result+="SamplerState "+std::string(FogDistanceName)+"Sampler"+Register(bindings.at(std::string(FogDistanceName)+"Sampler"))+";\n";
    result+=FragmentFunctions;
    result+=R"HLSL(
EngineFragmentOutput EngineFragmentEntry(PSInput input) {
    float cameraDistance=0;
    if(EngineTargetKind.x>0.5 && EngineIsFullscreen==0)
        cameraDistance=EngineStableLength(input.worldPosition-EngineCameraPosition.xyz);
    if(EngineTargetKind.x>0.5) {
        float4 baseColor=input.color;
)HLSL";
    const bool standardTexture=std::any_of(model.declarations.begin(),model.declarations.end(),[](const auto& declaration) {
        return declaration.kind==DeclarationKind::Texture && declaration.name=="texture";
    });
    if(standardTexture) result+="        baseColor*="+Sampling::Function("texture",false)+"(textureSampler,input.uv);\n";
    result+=R"HLSL(
        if(EngineFogRange.w>0.5) {
            float fogDistance=cameraDistance;
            if(EngineIsFullscreen!=0)
                fogDistance=EngineFogDistance.SampleLevel(EngineFogDistanceSampler,input.position.xy*EngineTargetInvSize,0);
            // A fullscreen triangle is not a world surface. An unwritten
            // history pixel must retain its original, un-fogged tint/material.
            if(EngineIsFullscreen==0 || fogDistance>0)
                baseColor=EngineFogBase(baseColor,fogDistance);
        }
        input.color=baseColor;
    }
    // PSMain is the user's ordinary function. It may intentionally disregard
    // input.color: there is no second Fog application after its return value.
    float4 straight=PSMain(input);
    EngineFragmentOutput output;
    output.color=float4(straight.rgb*straight.a,straight.a);
    // Fullscreen preserves target depth/distance through the pipeline mask.
    // Zero here permits DXC to discard history entirely when Fog is unobserved.
    output.distance=cameraDistance;
    return output;
}
)HLSL";
    return result;
}
std::string Generate(const Model& model,const Bindings& bindings,bool vertex) {
    struct Edit {std::size_t begin,end;std::string text;};
    std::vector<Edit> edits;
    edits.reserve(model.declarations.size()+model.sampling.size());
    for(const auto& d:model.declarations) edits.push_back({d.bindingOffset,d.bindingOffset,Register(bindings.at(d.name))});
    for(const auto& call:model.sampling) edits.push_back({call.begin,call.end,Sampling::Function(call.texture,call.explicitLod)});
    std::sort(edits.begin(),edits.end(),[](const auto& a,const auto& b){return a.begin<b.begin;});
    std::string result=Sampling::Prototypes(model); result.reserve(model.source.size()+16384);
    std::size_t cursor{};
    for(const auto& edit:edits) {
        if(edit.begin<cursor || edit.end<edit.begin || edit.end>model.source.size()) throw std::logic_error("invalid Shader semantic edit span");
        result.append(model.source,cursor,edit.begin-cursor);
        result+=edit.text;
        cursor=edit.end;
    }
    result.append(model.source,cursor);
    if(const auto found=bindings.find(Sampling::BufferName);found!=bindings.end()) {
        result+="\n#line 1 \"Engine.generated.hlsl\"\ncbuffer "+std::string(Sampling::BufferName)+Register(found->second)+" {\n";
        for(const auto& d:model.declarations) if(d.kind==DeclarationKind::Texture && d.textureRole==TextureRole::Color)
            result+=" float4 EngineRegion_"+d.name+"; float4 EngineInfo_"+d.name+";\n";
        result+="};\n";
        result+=Sampling::Definitions(model);
    }
    result+=DrawState(bindings);
    if(vertex) {
        if(!model.hasVertex) result+=DefaultVertex;
        result+=Adapter;
    } else result+=Fragment(model,bindings);
    return result;
}

ComPtr<ID3D12ShaderReflection> Reflect(Compiler& compiler,IDxcResult* result) {
    ComPtr<IDxcBlob> data;
    HR(result->GetOutput(DXC_OUT_REFLECTION,IID_PPV_ARGS(&data),nullptr),"read DXIL reflection blob");
    if(!data) throw std::runtime_error("DXC emitted no reflection data");
    DxcBuffer input{data->GetBufferPointer(),data->GetBufferSize(),0};
    ComPtr<ID3D12ShaderReflection> reflection;
    HR(compiler.utils->CreateReflection(&input,IID_PPV_ARGS(&reflection)),"reflect DXIL");
    return reflection;
}
Active Usage(ID3D12ShaderReflection* reflection) {
    D3D12_SHADER_DESC desc{}; HR(reflection->GetDesc(&desc),"read Shader reflection descriptor");
    Active active;
    for(UINT i=0;i<desc.BoundResources;++i) {
        D3D12_SHADER_INPUT_BIND_DESC binding{};
        HR(reflection->GetResourceBindingDesc(i,&binding),"read Shader binding");
        if(binding.BindCount!=1) throw std::invalid_argument("Shader resource arrays are unsupported");
        if(binding.Type==D3D_SIT_TEXTURE) {
            if(binding.Dimension!=D3D_SRV_DIMENSION_TEXTURE2D) throw std::invalid_argument("only Texture2D is supported");
            active.textures.emplace(binding.Name);
        } else if(binding.Type==D3D_SIT_SAMPLER) {
            std::string name=binding.Name;
            if(!name.ends_with("Sampler")) throw std::invalid_argument("unpaired Shader sampler");
            name.resize(name.size()-7); active.textures.emplace(std::move(name));
        } else if(binding.Type==D3D_SIT_CBUFFER) active.buffers.emplace(binding.Name);
        else throw std::invalid_argument("unsupported Shader buffer/resource binding: "+std::string(binding.Name));
    }
    if(active.textures.size()>16 || active.buffers.size()>4) throw std::length_error("Shader exceeds SDL_GPU stage limits (16 textures, 4 uniform buffers)");
    return active;
}
ValueType ReflectedType(ID3D12ShaderReflectionType* type) {
    D3D12_SHADER_TYPE_DESC desc{}; HR(type->GetDesc(&desc),"read uniform type");
    if(desc.Elements || desc.Members) throw std::invalid_argument("uniform arrays/structures are unsupported");
    if(desc.Type==D3D_SVT_FLOAT) {
        if(desc.Class==D3D_SVC_MATRIX_COLUMNS && desc.Rows==4 && desc.Columns==4) return ValueType::Matrix4;
        if(desc.Rows==1 && desc.Class==D3D_SVC_SCALAR && desc.Columns==1) return ValueType::Float;
        if(desc.Rows==1 && desc.Class==D3D_SVC_VECTOR) {
            if(desc.Columns==2) return ValueType::Float2;
            if(desc.Columns==3) return ValueType::Float3;
            if(desc.Columns==4) return ValueType::Float4;
        }
    }
    if(desc.Class==D3D_SVC_SCALAR && desc.Rows==1 && desc.Columns==1) {
        if(desc.Type==D3D_SVT_INT) return ValueType::Int;
        if(desc.Type==D3D_SVT_UINT) return ValueType::UInt;
    }
    throw std::invalid_argument("uniform type cannot be mapped to the Engine Shader ABI");
}
Stage BuildStage(Compiler& compiler,const Model& model,bool vertex) {
    const char* entry=vertex?"EngineVertexEntry":"EngineFragmentEntry";
    const std::vector<std::wstring> options{Wide("./"+compiler.path),L"-T",vertex?L"vs_6_0":L"ps_6_0",
        L"-E",Wide(entry),L"-HV",L"2021",L"-O3",L"-Zpc",L"-Ges"};
    const auto initial=Assign(model,vertex,nullptr);
    auto result=compiler.Run(Generate(model,initial,vertex),options,entry);
    auto reflection=Reflect(compiler,result.Get());
    const auto active=Usage(reflection.Get());
    const auto bindings=Assign(model,vertex,&active);
    // Compact only after semantic compilation identifies active resources;
    // declarations optimized away remain in Model for later Lua validation.
    result=compiler.Run(Generate(model,bindings,vertex),options,entry);
    reflection=Reflect(compiler,result.Get());
    const auto actual=Usage(reflection.Get());
    if(actual.textures!=active.textures || actual.buffers!=active.buffers)
        throw std::runtime_error("Shader resource usage changed during binding assignment");
    Stage stage; stage.entry=entry;
    ComPtr<IDxcBlob> code;
    HR(result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&code),nullptr),"read Shader bytecode");
    if(!code || code->GetBufferSize()>CodeLimit) throw std::length_error("Shader bytecode exceeds 16 MiB");
    const auto* bytes=static_cast<const std::uint8_t*>(code->GetBufferPointer());
    stage.code.assign(bytes,bytes+code->GetBufferSize());
    D3D12_SHADER_DESC desc{}; HR(reflection->GetDesc(&desc),"read final Shader descriptor");
    for(UINT i=0;i<desc.BoundResources;++i) {
        D3D12_SHADER_INPUT_BIND_DESC value{};
        HR(reflection->GetResourceBindingDesc(i,&value),"read final Shader binding");
        const auto found=bindings.find(value.Name);
        if(found==bindings.end() || found->second.slot!=value.BindPoint || found->second.space!=value.Space)
            throw std::runtime_error("DXIL register reflection disagrees with generated Shader binding: "+std::string(value.Name));
    }
    for(const auto& name:active.textures) stage.textures.push_back({name,bindings.at(name).slot,!vertex && name==FogDistanceName});
    std::sort(stage.textures.begin(),stage.textures.end(),[](const auto& a,const auto& b){ return a.slot<b.slot; });
    for(const auto& name:active.buffers) {
        auto* buffer=reflection->GetConstantBufferByName(name.c_str());
        D3D12_SHADER_BUFFER_DESC bd{}; HR(buffer->GetDesc(&bd),"read constant buffer layout");
        BufferBinding binding{name,bindings.at(name).slot,bd.Size,{}};
        if(!bd.Size || bd.Size>64*1024) throw std::length_error("Shader uniform buffer exceeds supported size");
        for(UINT i=0;i<bd.Variables;++i) {
            auto* variable=buffer->GetVariableByIndex(i);
            D3D12_SHADER_VARIABLE_DESC vd{}; HR(variable->GetDesc(&vd),"read uniform layout");
            const auto type=ReflectedType(variable->GetType());
            if(vd.StartOffset>bd.Size || vd.Size>bd.Size-vd.StartOffset) throw std::runtime_error("invalid reflected uniform extent");
            if(name=="Params") {
                const auto declared=std::find_if(model.parameters.begin(),model.parameters.end(),[&](const auto& p){ return p.name==vd.Name; });
                if(declared==model.parameters.end() || declared->type!=type) throw std::runtime_error("uniform reflection disagrees with source declaration");
            }
            binding.fields.push_back({vd.Name,type,vd.StartOffset,vd.Size,(vd.uFlags&D3D_SVF_USED)!=0});
        }
        stage.buffers.push_back(std::move(binding));
    }
    std::sort(stage.buffers.begin(),stage.buffers.end(),[](const auto& a,const auto& b){ return a.slot<b.slot; });
    return stage;
}
#endif
}

Compiled Compile(std::shared_ptr<const File::FileSystem> files,std::string_view path,const std::function<bool()>& cancelled) {
    if(!files) throw std::invalid_argument("Shader compiler requires VFS");
    const auto normalized=files->NormalizePath(path);
    if(!normalized || normalized->empty()) throw std::invalid_argument("invalid Shader source path");
    CheckCancelled(cancelled);
#ifdef _WIN32
    Compiler compiler(files,*normalized,cancelled);
    auto preprocessed=compiler.Preprocess();
    if(!compiler.includes->builtinIncluded)
        throw std::invalid_argument(*normalized+":1:1: Shader must #include \"Engine.hlsl\" for the public ABI");
    Compiled result; result.path=*normalized;
    result.model=Parse(std::move(preprocessed),*normalized);
    result.model.sampling=Sampling::Analyze(result.model,cancelled);
    result.vertex=BuildStage(compiler,result.model,true);
    result.fragment=BuildStage(compiler,result.model,false);
    CheckCancelled(cancelled);
    if(result.ResidentBytes()>64*MiB) throw std::length_error("prepared Shader exceeds 64 MiB");
    return result;
#else
    throw std::runtime_error("Runtime Shader compilation is not yet available for this platform");
#endif
}
}
