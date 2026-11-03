# Static DXC source integration

The `source` directory is Microsoft's DirectXShaderCompiler release `v1.9.2609`,
commit `01b62ad47db7dee3dd33f90e7339cf9e860b3f93`, obtained from:

https://codeload.github.com/microsoft/DirectXShaderCompiler/tar.gz/01b62ad47db7dee3dd33f90e7339cf9e860b3f93

Archive SHA-256:
`c48c223df13ba425fd9beda9843754defbcd379a3811a6a0e590110be57168c7`.
It was extracted without nested Git metadata. Original authorship, source
licenses, `source/LICENSE.TXT`, and `source/ThirdPartyNotices.txt` are retained.
The previous Windows SDK DLL deployment helper and redistribution records have
been retired; this source tree identifies the current static implementation.

Local modifications are recorded in `patches/static-embedding.patch`:

- An opt-in `ENGINE_DXC_STATIC` branch builds the actual `dxcompiler` source as
  a static library, omits DLL resources/export definitions, and removes the
  DLL import/export decorations for its factory entry points.
- `EngineDxcInitialize` and `EngineDxcShutdown` perform the same initialization
  and cleanup previously owned by Windows `DllMain`. Initialization happens
  once; shutdown is terminal. The host must finish all compiler jobs and
  destroy every DXC COM object before shutdown, with no concurrent calls.
- The source compiler, reflection, diagnostics, internal validator, and retail
  DXIL hash implementation are unchanged. No validation-skip option, preview
  bypass, DLL extraction, or runtime download is used.

The integration code in `../../tools/DxcStatic.cmake` and
`../cmake/dxc` is project-owned. It builds the dependency in a separate CMake
tree, walks the actual static target link graph, and uses Microsoft's librarian
to combine implementation archives (including DIA GUID constants) into
`static/dxc-static.lib`. This is an implementation archive, not a renamed import
library. Optional SPIR-V/Metal code generation, DXBC conversion, and upstream
tests are disabled; no external submodule is needed for this Windows DXIL path.
DXC's global allocation overrides are disabled. Release uses `/MT`, Debug
uses `/MTd`, and Debug iterators match the normal host MSVC ABI.

The build requires the Visual Studio ATL component. Its installed ATL headers
and `src/atl/atls/atlbase.cpp` are used as build inputs; that base module is
compiled with the selected compiler and CRT. No ATL source is copied into this
repository, and no differently built `atls.lib` or ATL DLL is linked. The
`ENGINE_DXC_ATL_INCLUDE_DIR` dependency-build cache entry can select an installed
ATL location; otherwise the current toolset and installed sibling toolsets are
searched.

The cache option `ENGINE_DXC_BINARY_DIR` selects the isolated build directory
for a single configuration. Do not share that directory between simultaneous
Debug and Release builds. Configure/build uses four compile jobs and one link
job. Nothing is downloaded by the build integration.

`../cmake/dxc/Probe.cpp` checks direct static factory calls, successful shader
model 6.0 compilation, a recomputed retail hash with preview bypass rejected,
internal validation, resource/uniform reflection, invalid-HLSL diagnostics,
four concurrent compiler workers, process module paths while COM objects are
alive and after workers finish, and the terminal shutdown contract.
Native D3D12 rendering remains an engine-level
acceptance check; the probe does not substitute for it.

Release verification on 2026-11-04 used MSVC 19.44.35228, SDK 10.0.26100.0,
and the installed Visual Studio ATL 14.51 headers/base-module source, compiled
with that same 19.44 compiler and `/MT`. The probe passed with exit code 0
from a directory containing only its EXE and with `PATH` restricted to the
Windows system directory. PE direct imports were exactly `ole32.dll`,
`OLEAUT32.dll`, `ADVAPI32.dll`, and `KERNEL32.dll`; all observed runtime modules
other than the EXE were Windows system modules. The executable was 17,495,552
bytes; the combined development-only static archive was 1,304,546,606 bytes.
The static archive and build intermediates are not distribution artifacts.

Debug verification on the same date/toolchain used `/MTd` with normal Debug
iterators and passed the same isolated-runtime checks with exit code 0. Its
direct imports were the four Release system libraries above plus `USER32.dll`.
The Debug EXE was 65,489,408 bytes and its development-only combined archive
was 1,553,839,070 bytes. There were no compiler DLLs or Visual C++ redistributable
DLLs in either configuration's imports or observed process modules. Windows
system components such as `ucrtbase.dll`, loaded transitively by Windows APIs,
remain ordinary operating-system dependencies.

Local evidence is under `build/dxc-static-probe` (Release) and
`build/dxc-static-probe-debug` (Debug): `probe-output.log`,
`probe-dependents.log`, and `probe-imports.log`. Each `isolated-runtime`
subdirectory contains only the verified EXE. These paths are build artifacts
and are intentionally excluded from source control.

These local adaptations were prepared with Codex assistance. No upstream pull
request is made by this integration.
