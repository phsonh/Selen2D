# Windows runtime checks

These opt-in developer checks are not copied into the runtime package. They
use the production DXC loader, VFS include handler and Shader compiler without
initializing SDL or a GPU. Ordinary source builds do not need `tests/` or `devs/`.

Build using the same entry point as CI:

```powershell
python -m pip install -r tools/pack/requirements-launch.txt
./tools/build.ps1 -Configuration Release -PublicRelease -StageRelease -Checks
```

Run the CPU checks from a Visual Studio x64 Developer PowerShell. Each output
directory must be new; existing evidence is never deleted. The loader matrix
builds its own small malformed DLLs, not replacements from an SDK or PATH:

```powershell
python tools/checks/check_dxc.py --bin build/public-release/bin --output build/checks/dxc-01 --dumpbin (Get-Command dumpbin.exe).Source
```

Release staging writes `release.json`, `package-files.json` and the raw DLL PE
inspection under its unique `build/release-work/` directory. Pass the manifest
from the same staging operation as the ZIP:

```powershell
python tools/checks/check_package.py --zip releases/selen2d-v2.3.0-windows-x64.zip --manifest build/release-work/<staging-directory>/package-files.json --output build/checks/package-01
```

Add `--smoke` for the existing finite-frame and Orbit Garden self-test runs.
All engine launches in these automated checks pass `--no-error-dialog` before
any application `--` separator, including negative startup cases. This makes
automation independent of interactive-desktop or inherited-output detection.
The `--smoke` option requires a working Windows D3D12 device. It extracts to a new
directory, uses a different working directory and a System32-only child PATH,
retains stdout/stderr, and requires the example's actual JSON/PNG results.

The workflow has three independent gates: tracked first-party hygiene, a
blocking Release build plus CPU compiler/loader/package checks, and a
non-blocking D3D12 smoke job. Hosted GPU/WARP availability is not established;
only the GPU job is allowed to fail. Local checks or YAML validation do not
constitute a successful GitHub Actions run.

The runtime DXC cache is limited to the isolated Release dependency build
directory (about 3.23 GB measured locally, including objects and symbols).
The key includes the runner image, actual compiler, SDK, pinned DXC revision,
source/integration inputs and configuration. Nothing rewrites source timestamps
to force a cache hit; a checkout or toolchain change may require recompilation.
