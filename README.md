# Selen2D
A lightweight 2D game engine.

The `v2-development` branch is the staged implementation of the second API
revision. Its development version is 2.0.1; this does not mean that every planned
interface is available yet. `v2.0.0` remains the published first-revision baseline.
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
`-Test` explicitly opts into locally installed tests. During the staged API
migration, the second-revision contract tests intentionally report missing or
conflicting behavior rather than accepting old aliases.

Packaging is an explicit Release-only step:

```powershell
cmake --build build/release --target stage_release
```

Ordinary Debug and Release builds never overwrite the official package. The
development package contains the executable, Lua entry/example and documentation;
source tools and private test output are not included.
