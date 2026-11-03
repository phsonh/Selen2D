#include "EngineDxcStatic.h"
#include <d3d12shader.h>
#include <dxc/dxcapi.h>
#include <dxc/DxilContainer/DxilContainer.h>
#include <dxc/DxilHash/DxilHash.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void Check(HRESULT result, const char* message) {
    Require(SUCCEEDED(result), message);
}
static ComPtr<IDxcResult> Compile(IDxcCompiler3* compiler, const char* source) {
    DxcBuffer input{source, std::strlen(source), DXC_CP_UTF8};
    const wchar_t* args[]{L"static-probe.hlsl", L"-T", L"ps_6_0", L"-E", L"main", L"-HV", L"2021", L"-Ges", L"-O3"};
    ComPtr<IDxcResult> result;
    Check(compiler->Compile(&input, args, ARRAYSIZE(args), nullptr, IID_PPV_ARGS(&result)), "Compile API failed");
    return result;
}
static void VerifyModules();
static void ExerciseCompiler(bool verifyModules = false) {
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcValidator> validator;
    Check(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "Create utils failed");
    Check(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "Create compiler failed");
    Check(DxcCreateInstance(CLSID_DxcValidator, IID_PPV_ARGS(&validator)), "Create internal validator failed");
    auto result = Compile(compiler.Get(),
        "cbuffer Probe : register(b0) { float4 tint; };\n"
        "Texture2D<float4> image : register(t0); SamplerState sampler0 : register(s0);\n"
        "float4 main(float4 p : SV_Position) : SV_Target { return tint + image.SampleLevel(sampler0, float2(.5,.5), 0); }\n");
    HRESULT status = E_FAIL;
    Check(result->GetStatus(&status), "Cannot query compile status");
    Check(status, "Valid ps_6_0 compilation failed");
    ComPtr<IDxcBlob> object;
    Check(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr), "No compiled DXIL");
    Require(object->GetBufferSize() > 20, "Truncated DXIL");
    const auto* bytes = static_cast<const unsigned char*>(object->GetBufferPointer());
    Require(std::memcmp(bytes, "DXBC", 4) == 0, "Not a DXIL container");
    const auto* container = hlsl::IsDxilContainerLike(bytes, object->GetBufferSize());
    Require(container && container->ContainerSizeInBytes == object->GetBufferSize(), "Invalid DXIL container size");
    constexpr auto hashStart = offsetof(hlsl::DxilContainerHeader, Version);
    BYTE retailHash[DXIL_CONTAINER_HASH_SIZE]{};
    ComputeHashRetail(bytes + hashStart, container->ContainerSizeInBytes - hashStart, retailHash);
    Require(std::memcmp(container->Hash.Digest, retailHash, sizeof(retailHash)) == 0, "DXIL retail hash mismatch");
    Require(std::memcmp(container->Hash.Digest, hlsl::PreviewByPassHash.Digest, sizeof(retailHash)) != 0, "Preview validation bypass detected");
    ComPtr<IDxcOperationResult> validation;
    Check(validator->Validate(object.Get(), DxcValidatorFlags_Default, &validation), "Validate API failed");
    Check(validation->GetStatus(&status), "Cannot query validator status");
    Check(status, "Internal DXIL validation failed");
    ComPtr<IDxcBlob> reflected;
    Check(result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&reflected), nullptr), "No reflection blob");
    DxcBuffer reflectionBuffer{reflected->GetBufferPointer(), reflected->GetBufferSize(), 0};
    ComPtr<ID3D12ShaderReflection> reflection;
    Check(utils->CreateReflection(&reflectionBuffer, IID_PPV_ARGS(&reflection)), "CreateReflection failed");
    D3D12_SHADER_DESC description{};
    Check(reflection->GetDesc(&description), "Reflection GetDesc failed");
    Require(description.BoundResources == 3 && description.ConstantBuffers == 1, "Unexpected reflection bindings");
    D3D12_SHADER_VARIABLE_DESC variable{};
    Check(reflection->GetConstantBufferByName("Probe")->GetVariableByName("tint")->GetDesc(&variable), "Uniform reflection failed");
    Require(variable.Size == 16 && variable.StartOffset == 0, "Unexpected uniform layout");
    auto invalid = Compile(compiler.Get(), "float4 main() : SV_Target { return missing_symbol; }");
    Check(invalid->GetStatus(&status), "Cannot query invalid shader status");
    Require(FAILED(status), "Invalid HLSL unexpectedly succeeded");
    ComPtr<IDxcBlobUtf8> errors;
    Check(invalid->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr), "No HLSL diagnostics");
    Require(errors && std::string(errors->GetStringPointer(), errors->GetStringLength()).find("missing_symbol") != std::string::npos,
        "Missing source diagnostic");
    if (verifyModules) VerifyModules();
    std::printf("PASS: ps_6_0 compiled, retail hash present, internal validation, reflection/uniform layout, invalid HLSL diagnostic\n");
}
static void VerifyModules() {
    wchar_t systemDirectory[MAX_PATH]{};
    wchar_t executable[MAX_PATH]{};
    Require(GetSystemDirectoryW(systemDirectory, ARRAYSIZE(systemDirectory)) != 0, "Cannot locate Windows system directory");
    Require(GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable)) != 0, "Cannot locate probe executable");
    const std::wstring systemPrefix = std::wstring(systemDirectory) + L"\\";
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    Require(snapshot != INVALID_HANDLE_VALUE, "Cannot enumerate process modules");
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool forbidden = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            std::wprintf(L"MODULE: %ls\n", entry.szExePath);
            if (_wcsicmp(entry.szExePath, executable) != 0 &&
                _wcsnicmp(entry.szExePath, systemPrefix.c_str(), systemPrefix.size()) != 0) forbidden = true;
            if (_wcsicmp(entry.szModule, L"dxcompiler.dll") == 0 ||
                _wcsicmp(entry.szModule, L"dxil.dll") == 0 ||
                _wcsicmp(entry.szModule, L"libclang.dll") == 0 ||
                _wcsnicmp(entry.szModule, L"vcruntime", 9) == 0 ||
                _wcsnicmp(entry.szModule, L"msvcp140", 8) == 0) forbidden = true;
        } while (Module32NextW(snapshot, &entry));
    } else {
        CloseHandle(snapshot);
        throw std::runtime_error("Cannot read process modules");
    }
    CloseHandle(snapshot);
    Require(!forbidden, "Third-party compiler or CRT DLL loaded");
}
int main() {
    try {
        Check(EngineDxcInitialize(), "Static DXC initialization failed");
        ExerciseCompiler(true);
        std::exception_ptr failures[4];
        std::thread workers[4];
        for (unsigned i = 0; i < 4; ++i) {
            workers[i] = std::thread([&, i] {
                try { ExerciseCompiler(); }
                catch (...) { failures[i] = std::current_exception(); }
            });
        }
        for (auto& worker : workers) worker.join();
        for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
        VerifyModules();
        EngineDxcShutdown();
        Require(FAILED(EngineDxcInitialize()), "Shutdown must be terminal");
        std::puts("PASS: static DXC lifetime and no compiler or Visual C++ redistributable DLLs");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        EngineDxcShutdown();
        return 1;
    }
}
