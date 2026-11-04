# Selen2D

![Selen2D Banner](src/res/image/selen2d-banner.png)
A lightweight 2D game engine.
The `v2-development` branch implements the frozen second API revision. Its
development version is 2.0.1, not a newly published release. Windows x64/D3D12
validation and remaining platform limits are recorded separately from API coverage. `v2.0.0` remains the published first-revision baseline.
The editor definitions in `doc/api` describe the interfaces actually implemented
in the current source, and are updated alongside each module.

## Windows build

Use Visual Studio C++ tools (MSVC 14.44, Windows SDK and ATL), CMake, Ninja and
Python for the **build**. All runtime dependencies, including DXC, are linked
statically; the running engine and example do not require Python or compiler DLLs.

```powershell
./tools/build.ps1 -Configuration Release
./tools/build.ps1 -Configuration Debug
```

Normal builds do not require the local, ignored `tests/` or `devs/` directories.
`-Test` explicitly opts into locally installed tests. The second-revision tests
check the current contract and do not accept obsolete aliases. Private integration
fixtures are not silently required by a normal checkout or build.

Packaging is an explicit Release-only step:

```powershell
cmake --build build/release --target stage_release
```

Ordinary Debug and Release builds never overwrite the official package. The
development package contains the executable, Lua example and documentation;
source tools and private test output are not included.

The default executable starts `example/main.lua` directly from its compiled
`launch.h` configuration. It authorizes the `example/` and `output/` virtual
ranges; it does not expose the whole executable directory. There is no external
startup file or runtime configuration override. Changing the entry, mounts or
directory rules requires rebuilding the executable; editing an authorized Lua
file in the default development layout only requires restarting it.

Stage C adds the File/VFS contract, compiled startup configuration, and Python
tools for SLPK v1 packages and authenticated SLPK v2 packages. The Python
packages in `tools/requirements-tools.txt` are needed only when running those
source tools. Ordinary engine builds need the build tools listed above, without
those extra Python packages; the running engine does not need Python.
Resource options, atomic Loader publication, explicit audio players, Canvas dual
entry points, Mesh versions, Engine.hlsl sampling and ordinary text are now
implemented. The integration acceptance is recorded separately from API coverage.

See [running the example](doc/guide/getting-started.md) and
[File, packages and compiled startup configuration](doc/guide/file-launch.md).
The complete Lua example includes menu, loading, play, pause and cleanup flows.
Its custom Shader uses the built-in `Engine.hlsl` declarations described
in the [Shader guide](doc/guide/shader.md).
