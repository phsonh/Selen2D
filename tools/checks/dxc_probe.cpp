// Optional Windows CPU-only check. Uses the production loader and compiler;
// never links a DXC import library, initializes a GPU, or changes DLL search paths.
#include "core/modules/Graphic/shader/DxcRuntime.h"
#include "core/modules/Graphic/shader/Compiler.h"
#include "core/modules/File/Vfs.h"
#include "core/runtime/Log/Log.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <psapi.h>
#include <d3d12shader.h>
#include <dxc/dxcapi.h>
#include <dxc/dxcisense.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace Shader=core::modules::Graphic::Shader;
namespace Dxc=Shader::DxcRuntime;
namespace File=core::modules::File;
namespace Log=core::runtime::Log;
namespace fs=std::filesystem;
namespace {
void Require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
void HR(HRESULT result,const char* message) {
    if(FAILED(result)) throw std::runtime_error(std::string(message)+" (HRESULT "+std::to_string(result)+")");
}
std::string Utf8(const std::wstring& value) {
    const auto length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    Require(length>0,"UTF-8 path conversion failed");
    std::string result(static_cast<std::size_t>(length),'\0');
    Require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),length,nullptr,nullptr)>0,"UTF-8 path conversion failed");
    return result;
}
std::string Quote(std::string_view value) {
    constexpr char hex[]="0123456789abcdef";
    std::string result="\"";
    for(unsigned char c:value) {
        if(c=='"' || c=='\\') {result+='\\';result+=static_cast<char>(c);}
        else if(c<32) {result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}
        else result+=static_cast<char>(c);
    }
    return result+'"';
}
void Record(std::string_view json) { std::printf("DXC_CHECK %.*s\n",static_cast<int>(json.size()),json.data());std::fflush(stdout); }
void Pass(const char* name) { Record("{\"kind\":\"case\",\"name\":"+Quote(name)+",\"ok\":true}"); }
std::wstring ModulePath(HMODULE module) {
    std::vector<wchar_t> buffer(256);
    for(;;) {
        const auto length=GetModuleFileNameW(module,buffer.data(),static_cast<DWORD>(buffer.size()));
        Require(length>0,"module filename query failed");
        if(length<buffer.size()) return {buffer.data(),length};
        Require(buffer.size()<1024*1024,"unbounded module filename");buffer.resize(buffer.size()*2);
    }
}
void Modules(const char* phase) {
    const auto executable=ModulePath(nullptr);
    const auto expected=(fs::path(executable).parent_path()/L"dxcompiler.dll").native();
    std::vector<wchar_t> system(256);
    auto length=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));
    Require(length>0,"system directory query failed");
    if(length>=system.size()) {system.resize(static_cast<std::size_t>(length)+1);length=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));}
    Require(length>0&&length<system.size(),"system directory truncated");
    const std::wstring systemPrefix=std::wstring(system.data(),length)+L"\\";
    // Toolhelp can fail while constructing its MAX_PATH-sized module records
    // for a long deployment path. Enumerate handles through Kernel32 instead;
    // the actual filename query below always uses a growing buffer.
    std::vector<HMODULE> modules(64);DWORD needed{};
    for(;;) {
        Require(K32EnumProcessModules(GetCurrentProcess(),modules.data(),static_cast<DWORD>(modules.size()*sizeof(HMODULE)),&needed)!=FALSE,"module enumeration failed");
        if(needed<=modules.size()*sizeof(HMODULE)) {modules.resize(needed/sizeof(HMODULE));break;}
        modules.resize(needed/sizeof(HMODULE)+16);
    }
    bool found=false,dxil=false,forbidden=false;
    for(const auto module:modules) {
        const auto path=ModulePath(module);
        const auto name=fs::path(path).filename().native();
        const bool local=_wcsicmp(path.c_str(),expected.c_str())==0;
        const bool self=_wcsicmp(path.c_str(),executable.c_str())==0;
        const bool os=_wcsnicmp(path.c_str(),systemPrefix.c_str(),systemPrefix.size())==0;
        found|=local;
        dxil|=_wcsicmp(name.c_str(),L"dxil.dll")==0;
        forbidden|=!local&&!self&&!os;
        const bool redistributable=_wcsnicmp(name.c_str(),L"msvcp",5)==0 && !(os&&_wcsicmp(name.c_str(),L"msvcp_win.dll")==0);
        forbidden|=redistributable||_wcsnicmp(name.c_str(),L"vcruntime",9)==0||
            _wcsicmp(name.c_str(),L"libclang.dll")==0||_wcsnicmp(name.c_str(),L"LLVM",4)==0;
        Record("{\"kind\":\"module\",\"phase\":"+Quote(phase)+",\"path\":"+Quote(Utf8(path))+"}");
    }
    Record("{\"kind\":\"modules\",\"phase\":"+Quote(phase)+",\"expected\":"+Quote(Utf8(expected))+
        ",\"found\":"+(found?"true":"false")+",\"dxil\":"+(dxil?"true":"false")+",\"forbidden\":"+(forbidden?"true":"false")+"}");
    Require(found&&!forbidden&&!dxil,"unexpected DXC, private LLVM, dynamic CRT, or non-system module");
}
ComPtr<IDxcResult> CompileRaw(IDxcCompiler3* compiler,const char* source) {
    DxcBuffer input{source,std::strlen(source),DXC_CP_UTF8};
    const wchar_t* args[]{L"public-check.hlsl",L"-T",L"ps_6_0",L"-E",L"main",L"-HV",L"2021",L"-Ges",L"-O3"};
    ComPtr<IDxcResult> result;
    HR(compiler->Compile(&input,args,ARRAYSIZE(args),nullptr,IID_PPV_ARGS(&result)),"Compile API");return result;
}
void Interfaces() {
    ComPtr<IDxcUtils> utils;ComPtr<IDxcCompiler3> compiler;ComPtr<IDxcValidator> validator;
    HR(Dxc::Create(CLSID_DxcUtils,IID_PPV_ARGS(&utils)),"create utils");
    HR(Dxc::Create(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)),"create compiler");
    HR(Dxc::Create(CLSID_DxcValidator,IID_PPV_ARGS(&validator)),"create validator");
    constexpr const char* source="cbuffer Probe:register(b0){float4 tint;};\n"
        "Texture2D<float4> image:register(t0);SamplerState imageSampler:register(s0);\n"
        "float4 main(float4 p:SV_Position):SV_Target{return tint+image.SampleLevel(imageSampler,float2(.5,.5),0);}";
    auto result=CompileRaw(compiler.Get(),source);HRESULT status=E_FAIL;
    HR(result->GetStatus(&status),"compile status");HR(status,"valid HLSL");
    ComPtr<IDxcBlob> object;HR(result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&object),nullptr),"DXIL output");
    Require(object&&object->GetBufferSize()>20&&std::memcmp(object->GetBufferPointer(),"DXBC",4)==0,"DXIL header");
    ComPtr<IDxcOperationResult> validation;
    HR(validator->Validate(object.Get(),DxcValidatorFlags_Default,&validation),"validator API");
    HR(validation->GetStatus(&status),"validator status");HR(status,"DXIL validation");
    ComPtr<IDxcBlob> reflected;HR(result->GetOutput(DXC_OUT_REFLECTION,IID_PPV_ARGS(&reflected),nullptr),"reflection output");
    DxcBuffer input{reflected->GetBufferPointer(),reflected->GetBufferSize(),0};
    ComPtr<ID3D12ShaderReflection> reflection;HR(utils->CreateReflection(&input,IID_PPV_ARGS(&reflection)),"reflection API");
    D3D12_SHADER_DESC desc{};HR(reflection->GetDesc(&desc),"reflection descriptor");
    Require(desc.BoundResources==3&&desc.ConstantBuffers==1,"reflection resource counts");
    D3D12_SHADER_VARIABLE_DESC field{};
    HR(reflection->GetConstantBufferByName("Probe")->GetVariableByName("tint")->GetDesc(&field),"reflection uniform");
    Require(field.StartOffset==0&&field.Size==16,"reflection layout");
    auto invalid=CompileRaw(compiler.Get(),"float4 main():SV_Target{return missing_symbol;}");
    HR(invalid->GetStatus(&status),"invalid compile status");Require(FAILED(status),"invalid HLSL was accepted");
    ComPtr<IDxcBlobUtf8> errors;HR(invalid->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&errors),nullptr),"invalid diagnostics");
    Require(errors&&std::string_view(errors->GetStringPointer(),errors->GetStringLength()).find("missing_symbol")!=std::string_view::npos,"missing invalid HLSL diagnostic");
    auto recovered=CompileRaw(compiler.Get(),source);HR(recovered->GetStatus(&status),"recovery status");HR(status,"recovery compile");
    Pass("public-utils-compiler-validator-reflection-recovery");
}
unsigned SampleMembers(IDxcCursor* cursor,unsigned depth=0) {
    Require(depth<128,"IntelliSense cursor depth");unsigned samples=0;DxcCursorKind kind{};
    HR(cursor->GetKind(&kind),"cursor kind");
    if(kind==DxcCursor_MemberRefExpr) {
        LPSTR name=nullptr;HR(cursor->GetSpelling(&name),"cursor spelling");
        if(name&&std::strcmp(name,"Sample")==0) ++samples;CoTaskMemFree(name);
    }
    for(unsigned skip=0;;) {
        struct Children {unsigned count{};IDxcCursor** data{};~Children(){for(unsigned i=0;i<count;++i)data[i]->Release();CoTaskMemFree(data);}} children;
        HR(cursor->GetChildren(skip,64,&children.count,&children.data),"cursor children");
        for(unsigned i=0;i<children.count;++i) samples+=SampleMembers(children.data[i],depth+1);
        skip+=children.count;if(children.count<64)break;
    }
    return samples;
}
void IntelliSense() {
    ComPtr<IDxcIntelliSense> sense;HR(Dxc::Create(CLSID_DxcIntelliSense,IID_PPV_ARGS(&sense)),"create IntelliSense");
    ComPtr<IDxcIndex> index;HR(sense->CreateIndex(&index),"IntelliSense index");
    constexpr char source[]="Texture2D<float4> image;SamplerState imageSampler;float4 main(float2 uv:TEXCOORD0):SV_Target{return image.Sample(imageSampler,uv);}";
    ComPtr<IDxcUnsavedFile> unsaved;HR(sense->CreateUnsavedFile("semantic-check.hlsl",source,static_cast<unsigned>(sizeof(source)-1),&unsaved),"unsaved source");
    auto* input=unsaved.Get();const char* args[]{"-HV","2021"};ComPtr<IDxcTranslationUnit> unit;
    HR(index->ParseTranslationUnit("semantic-check.hlsl",args,2,&input,1,DxcTranslationUnitFlags_UseCallerThread,&unit),"parse translation unit");
    unsigned count{};HR(unit->GetNumDiagnostics(&count),"semantic diagnostics");
    for(unsigned i=0;i<count;++i) {
        ComPtr<IDxcDiagnostic> diagnostic;HR(unit->GetDiagnostic(i,&diagnostic),"semantic diagnostic");
        DxcDiagnosticSeverity severity{};HR(diagnostic->GetSeverity(&severity),"semantic severity");
        Require(severity<DxcDiagnostic_Error,"IntelliSense parse error");
    }
    ComPtr<IDxcCursor> cursor;HR(unit->GetCursor(&cursor),"translation unit cursor");
    Require(SampleMembers(cursor.Get())==1,"IntelliSense Sample member count");Pass("public-intellisense-parse-cursor");
}
void Write(const fs::path& path,std::string_view text) {
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);
    stream.write(text.data(),static_cast<std::streamsize>(text.size()));stream.close();
    Require(static_cast<bool>(stream),"cannot write checker-owned fixture");
}
std::string StageJson(const Shader::Stage& stage) {
    std::ostringstream out;out<<"{\"entry\":"<<Quote(stage.entry)<<",\"codeBytes\":"<<stage.code.size()<<",\"textures\":[";
    bool first=true;
    for(const auto& texture:stage.textures) {
        if(!first)out<<',';first=false;
        out<<"{\"name\":"<<Quote(texture.name)<<",\"slot\":"<<texture.slot<<",\"engine\":"<<(texture.engine?"true":"false")<<'}';
    }
    out<<"],\"buffers\":[";first=true;
    for(const auto& buffer:stage.buffers) {
        if(!first)out<<',';first=false;
        out<<"{\"name\":"<<Quote(buffer.name)<<",\"slot\":"<<buffer.slot<<",\"size\":"<<buffer.size<<",\"fields\":[";
        bool fieldFirst=true;
        for(const auto& field:buffer.fields) {
            if(!fieldFirst)out<<',';fieldFirst=false;
            out<<"{\"name\":"<<Quote(field.name)<<",\"type\":"<<Quote(Shader::TypeName(field.type))<<",\"offset\":"<<field.offset
                <<",\"size\":"<<field.size<<",\"used\":"<<(field.used?"true":"false")<<'}';
        }
        out<<"]}";
    }
    out<<"]}";return out.str();
}
std::string Snapshot(const Shader::Compiled& compiled) {
    std::ostringstream out;out<<"{\"path\":"<<Quote(compiled.path)<<",\"parameters\":[";bool first=true;
    for(const auto& parameter:compiled.model.parameters) {if(!first)out<<',';first=false;out<<"{\"name\":"<<Quote(parameter.name)<<",\"type\":"<<Quote(Shader::TypeName(parameter.type))<<'}';}
    out<<"],\"declarations\":[";first=true;
    for(const auto& declaration:compiled.model.declarations) {
        if(!first)out<<',';first=false;
        out<<"{\"name\":"<<Quote(declaration.name)<<",\"kind\":"<<static_cast<int>(declaration.kind)<<",\"role\":"<<static_cast<int>(declaration.textureRole)
            <<",\"mainTexture\":"<<Quote(declaration.mainTexture)<<'}';
    }
    out<<"],\"samplingCalls\":"<<compiled.model.sampling.size()<<",\"vertex\":"<<StageJson(compiled.vertex)<<",\"fragment\":"<<StageJson(compiled.fragment)<<'}';
    return out.str();
}
template<class F> void Reject(const char* name,F&& action,const char* diagnostic=nullptr) {
    try {action();} catch(const std::exception& error) {
        if(diagnostic) Require(std::string_view(error.what()).find(diagnostic)!=std::string_view::npos,"wrong rejection diagnostic");
        Record("{\"kind\":\"rejection\",\"name\":"+Quote(name)+",\"message\":"+Quote(error.what())+"}");Pass(name);return;
    }
    throw std::runtime_error(std::string(name)+" unexpectedly succeeded");
}
void ProductionCompiler(const fs::path& root) {
    Require(root.is_absolute()&&!fs::exists(root),"fixture directory must be absolute and not exist");
    fs::create_directories(root/L"authorized");
    constexpr const char* good="#include \"Engine.hlsl\"\n#include \"common.hlsl\"\n"
        "Texture2D scene;SamplerState sceneSampler;\ncbuffer Params{float gain;float3 tint;float unused;};\n"
        "float4 PSMain(PSInput input):SV_Target0{return scene.Sample(sceneSampler,input.uv)*float4(ProbeTint(tint)*gain,1);}\n";
    Write(root/L"authorized/main.hlsl",good);
    Write(root/L"authorized/common.hlsl","float3 ProbeTint(float3 value){return value;}\n");
    Write(root/L"unauthorized.hlsl","float4 OutsideValue(){return float4(1,0,1,1);}\n");
    struct Bad {const char* name;const char* source;const char* diagnostic;};
    const Bad invalid[]{
        {"invalid-hlsl","#include \"Engine.hlsl\"\nfloat4 PSMain(PSInput i):SV_Target0{return missing_symbol;}","missing_symbol"},
        {"missing-abi","float4 PSMain(float2 uv:TEXCOORD0):SV_Target0{return 1;}","Engine.hlsl"},
        {"include-traversal","#include \"Engine.hlsl\"\n#include \"../unauthorized.hlsl\"\nfloat4 PSMain(PSInput i):SV_Target0{return OutsideValue();}","strict relative"},
        {"include-absolute","#include \"Engine.hlsl\"\n#include \"C:/unauthorized.hlsl\"\nfloat4 PSMain(PSInput i):SV_Target0{return 1;}","strict relative"},
        {"include-unauthorized","#include \"Engine.hlsl\"\n#include \"unauthorized.hlsl\"\nfloat4 PSMain(PSInput i):SV_Target0{return OutsideValue();}",nullptr},
        {"reserved-line","#line 1 \"Engine.hlsl\"\nfloat4 PSMain():SV_Target0{return 1;}","#line"},
        {"reserved-name","#include \"Engine.hlsl\"\nfloat4 PSMain(PSInput i):SV_Target0{float EngineLocal=1;return EngineLocal;}","reserved"},
        {"color-load","#include \"Engine.hlsl\"\nTexture2D scene;SamplerState sceneSampler;float4 PSMain(PSInput i):SV_Target0{return scene.Load(int3(0,0,0));}",nullptr},
        {"wrong-sampler","#include \"Engine.hlsl\"\nTexture2D scene,other;SamplerState sceneSampler,otherSampler;float4 PSMain(PSInput i):SV_Target0{return scene.Sample(otherSampler,i.uv);}",nullptr}
    };
    for(const auto& value:invalid) Write(root/L"authorized"/(std::string(value.name)+".hlsl"),value.source);
    auto files=std::make_shared<File::FileSystem>(root,std::vector<File::MountSpec>{},
        std::vector<File::DirectorySpec>{{root,"authorized","shaders"}},std::nullopt);
    const auto compiled=Shader::Compile(files,"shaders/main.hlsl");
    Require(compiled.format==Shader::BytecodeFormat::Dxil,"production bytecode format");
    Require(compiled.vertex.entry=="EngineVertexEntry"&&compiled.fragment.entry=="EngineFragmentEntry","production entries");
    for(const auto* stage:{&compiled.vertex,&compiled.fragment}) Require(stage->code.size()>20&&std::memcmp(stage->code.data(),"DXBC",4)==0,"production DXIL header");
    Require(compiled.model.parameters.size()==3&&compiled.model.sampling.size()==1,"production declared metadata");
    Require(compiled.model.parameters[0].name=="gain"&&compiled.model.parameters[0].type==Shader::ValueType::Float&&
        compiled.model.parameters[1].name=="tint"&&compiled.model.parameters[1].type==Shader::ValueType::Float3&&
        compiled.model.parameters[2].name=="unused","production parameter order/types");
    const auto texture=std::find_if(compiled.fragment.textures.begin(),compiled.fragment.textures.end(),[](const auto& value){return value.name=="scene";});
    Require(texture!=compiled.fragment.textures.end()&&texture->slot==0&&!texture->engine,"production texture binding");
    const auto params=std::find_if(compiled.fragment.buffers.begin(),compiled.fragment.buffers.end(),[](const auto& value){return value.name=="Params";});
    Require(params!=compiled.fragment.buffers.end()&&params->size==32&&params->fields.size()==3,"production parameter buffer layout");
    Require(params->fields[0].name=="gain"&&params->fields[0].offset==0&&params->fields[0].size==4&&params->fields[0].used&&
        params->fields[1].name=="tint"&&params->fields[1].offset==4&&params->fields[1].size==12&&params->fields[1].used&&
        params->fields[2].name=="unused"&&params->fields[2].offset==16&&params->fields[2].size==4&&!params->fields[2].used,"production parameter fields");
    const auto snapshot=Snapshot(compiled);Write(root/L"snapshot.json",snapshot);Record("{\"kind\":\"snapshot\",\"value\":"+snapshot+"}");
    Pass("production-vfs-includes-sampling-reflection-metadata");
    for(const auto& value:invalid) Reject(value.name,[&]{(void)Shader::Compile(files,"shaders/"+std::string(value.name)+".hlsl");},value.diagnostic);
    Reject("unauthorized-root",[&]{(void)Shader::Compile(files,"unauthorized.hlsl");});
    Reject("cancelled",[&]{(void)Shader::Compile(files,"shaders/main.hlsl",[]{return true;});},"cancelled");
    const auto recovered=Shader::Compile(files,"shaders/main.hlsl");
    Require(Snapshot(recovered)==snapshot&&recovered.vertex.code==compiled.vertex.code&&recovered.fragment.code==compiled.fragment.code,"production recovery changed output");
    Pass("production-valid-invalid-valid-recovery");
    std::array<std::exception_ptr,4> failures{};std::array<std::jthread,4> workers;
    for(std::size_t i=0;i<workers.size();++i) workers[i]=std::jthread([&,i]{
        try {
            for(unsigned repeat=0;repeat<2;++repeat) {
                const auto concurrent=Shader::Compile(files,"shaders/main.hlsl");
                Require(Snapshot(concurrent)==snapshot&&concurrent.vertex.code==compiled.vertex.code&&concurrent.fragment.code==compiled.fragment.code,"concurrent production result changed");
                IntelliSense();
            }
        } catch(...) {failures[i]=std::current_exception();}
    });
    for(auto& worker:workers)worker.join();
    for(const auto& failure:failures)if(failure)std::rethrow_exception(failure);
    Pass("production-four-thread-repeated-compile");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc<2 || (std::wstring_view(argv[1])!=L"--load-only"&&std::wstring_view(argv[1])!=L"--self-test"&&std::wstring_view(argv[1])!=L"--sticky-repair") ||
       (std::wstring_view(argv[1])==L"--load-only"&&argc!=2) || (std::wstring_view(argv[1])!=L"--load-only"&&argc!=3)) {
        std::fputs("usage: dxc_probe --load-only | --self-test <new absolute fixture directory> | --sticky-repair <replacement DLL>\n",stderr);return 2;
    }
    if(!Log::Init()) return 2;
    Log::LuaInfo("DXC checker Lua-history sentinel");const auto history=Log::GetLuaText();
    try {
        try {Dxc::Initialize();} catch(const std::exception& error) {
            const std::string original=error.what();bool sticky=true;
            const bool repair=std::wstring_view(argv[1])==L"--sticky-repair";
            if(repair) {
                // The host supplies a fresh, checker-owned deployment. Never
                // overwrite a DLL: this mode only repairs an initially missing
                // file and verifies the production failure remains cached.
                const auto destination=fs::path(ModulePath(nullptr)).parent_path()/L"dxcompiler.dll";
                Require(!fs::exists(destination),"sticky repair requires an initially missing DLL");
                Require(fs::copy_file(fs::path(argv[2]),destination),"sticky repair copy failed");
            }
            for(unsigned i=0;i<4;++i) {
                try {Dxc::Initialize();sticky=false;}catch(const std::exception& repeated){sticky&=original==repeated.what();}
            }
            Record("{\"kind\":\"loader_failure\",\"message\":"+Quote(original)+",\"sticky\":"+(sticky?"true":"false")+
                ",\"repaired\":"+(repair?"true":"false")+",\"cache_unchanged\":"+(Log::GetLuaText()==history?"true":"false")+"}");
            Require(sticky&&Log::GetLuaText()==history,"loader failure changed state/history");throw;
        }
        Require(std::wstring_view(argv[1])!=L"--sticky-repair","sticky repair unexpectedly initialized without its local DLL");
        Modules("initialized");
        if(std::wstring_view(argv[1])==L"--self-test") {
            Interfaces();IntelliSense();ProductionCompiler(fs::path(argv[2]));Modules("after-interfaces-and-production-compile");
        }
        ComPtr<IDxcUtils> retained;HR(Dxc::Create(CLSID_DxcUtils,IID_PPV_ARGS(&retained)),"retained utils");
        Log::Shutdown();Dxc::Initialize();Require(Log::Init(),"logger reinitialization failed");
        ComPtr<IDxcBlobEncoding> blob;HR(retained->CreateBlob("alive",5,DXC_CP_UTF8,&blob),"retained COM after reinitialize");
        Require(blob->GetBufferSize()>=5&&std::memcmp(blob->GetBufferPointer(),"alive",5)==0,"retained COM result");
        Require(Log::GetLuaText()==history,"native DXC diagnostics polluted Lua history");
        Modules("retained-com-after-log-reinitialize");Pass("process-lifetime-and-native-only-logging");
        Record("{\"kind\":\"summary\",\"ok\":true}");Log::Shutdown();return 0;
    } catch(const std::exception& error) {
        Log::Error(error.what());Record("{\"kind\":\"summary\",\"ok\":false,\"message\":"+Quote(error.what())+"}");
        Log::Shutdown();return 1;
    }
}
