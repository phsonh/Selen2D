# DXC runtime source integration

The `source` directory is Microsoft's DirectXShaderCompiler release `v1.9.2609`,
commit `01b62ad47db7dee3dd33f90e7339cf9e860b3f93`, obtained from:

https://codeload.github.com/microsoft/DirectXShaderCompiler/tar.gz/01b62ad47db7dee3dd33f90e7339cf9e860b3f93

Source archive SHA-256:
`c48c223df13ba425fd9beda9843754defbcd379a3811a6a0e590110be57168c7`.
The pinned source is retained without nested Git metadata or local source
patches. Original authorship, `source/LICENSE.TXT`,
`source/ThirdPartyNotices.txt`, and nested component licenses remain intact.
The distributed DLL is built from this source. An official prebuilt ZIP or
Windows SDK redistribution record is not its source or license provenance.

Project-owned integration lives in `../../tools/cmake/DxcRuntime.cmake` and
`../cmake/dxc`. It builds the upstream `dxcompiler` shared-library target in an
isolated CMake tree. LLVM implementation libraries remain build intermediates;
the engine does not link the DXC import library. Optional SPIR-V/Metal code
generation, DXBC conversion and upstream tests are disabled. The integration
downloads nothing and disables DXC global allocation overrides. Release uses
`/MT`, Debug uses `/MTd`, with matching Debug iterators.

The Visual Studio ATL component is a build prerequisite. Its installed headers
and `src/atl/atls/atlbase.cpp` are used through external CMake wrappers. The base
module is compiled with the selected compiler and CRT. No ATL source is copied
into this repository, and no separately built `atls.lib` or ATL DLL is used.
`ENGINE_DXC_ATL_INCLUDE_DIR` may select an installed ATL location; otherwise the
current toolset and installed sibling toolsets are searched. Microsoft CRT and
ATL redistribution terms still apply to these Microsoft components.

`ENGINE_DXC_RUNTIME_BINARY_DIR` selects an isolated dependency build directory.
Debug, Release and public Release use separate directories and matching DLLs.
Existing historical build directories are retained. Ordinary engine-source or
launch-configuration edits do not require recompiling DXC. The build-time
`ENGINE_DXC_EXECUTABLE` is a separate SDK/PATH `dxc.exe` used to embed built-in
shader bytecode; that tool and SDK DLLs are not collected into the runtime.

The Windows x64 runtime directory contains `Selen2D.exe` and `dxcompiler.dll`.
The engine obtains its native executable directory, loads only that absolute
DLL path with restricted DLL-directory/System32 search flags, and resolves
`DxcCreateInstance`. Shader compilation, validation, reflection and semantic
IntelliSense use the same factory. The module remains loaded for process
lifetime so compiler jobs and COM objects cannot outlive its code. The runtime
does not search the current directory, PATH or an SDK for another compiler.

The Release source-build probe verified file and product version `1.9.2609.0`,
x64 PE format, and the `DxcCreateInstance` / `DxcCreateInstance2` exports.
Its direct imports were exactly `KERNEL32.dll`, `ole32.dll`, `OLEAUT32.dll`,
and `ADVAPI32.dll`. Compiler, utilities, internal validator, reflection and
IntelliSense checks passed, including invalid-HLSL diagnostics, four parallel
compiler workers and repeated factory use. Module snapshots with live compiler
objects and after workers completed found only the selected `dxcompiler.dll`
and Windows system modules; `dxil.dll` was not loaded. These are local Windows
checks, not a claim about every platform or configuration.

The Debug `/MTd` DLL was also inspected as a real x64 build with file/product
version `1.9.2609.0`. It imports the same four system libraries plus
`USER32.dll!UnregisterClassA`, used by the ATL base-module Debug cleanup.
The verifier permits `USER32.dll` only for Debug; the Release allowlist stays
at four libraries. Neither build imports a dynamic MSVC runtime or private
LLVM library. No Windows system DLL is copied into either runtime directory.

Release staging independently verifies the actual DLL's architecture, factory
export, pinned version and direct-import allowlist, then records its SHA-256,
size and full PE inspection alongside the package file manifest under
`build/release-work`. Only `dxcompiler.dll` is approved as an application DLL;
`dxil.dll`, MSVC runtime DLLs, import libraries, PDBs and compiler executables
are excluded. Windows components remain operating-system dependencies; a
system API may load other Windows modules transitively. A dependency change
requires fresh evidence and review, not an automatic widening of the package.

The source probe does not replace actual engine rendering and image regression
checks. The supported release remains Windows x64 / Direct3D 12. A native loader
boundary does not establish Linux, Vulkan, macOS, Metal, SPIR-V or MSL support.
No GitHub Actions result is implied by local validation.
