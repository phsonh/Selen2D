[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [ValidateRange(1, 64)][int]$Jobs = 8,
    [switch]$Test,
    [switch]$ConfigureOnly,
    [string]$ToolsetVersion = '14.44'
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ($Test -and -not (Test-Path -LiteralPath (Join-Path $taskRoot 'tests/CMakeLists.txt'))) {
    throw 'The -Test option requires local tests/CMakeLists.txt; tests are not included in this checkout.'
}
$taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs = & $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVs) { throw 'Visual Studio C++ tools are required.' }
Import-Module (Join-Path $taskVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $taskVs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64 -vcvars_ver=$ToolsetVersion"
$taskCMake = Join-Path $taskVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$taskNinja = Join-Path $taskVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja'
$env:PATH = "$taskNinja;$(Split-Path $taskCMake);$env:PATH"
$taskPreset = 'selen2d-' + $Configuration.ToLowerInvariant()
Push-Location -LiteralPath $taskRoot
try {
    $taskBuildTesting = if ($Test) { 'ON' } else { 'OFF' }
    & $taskCMake --preset $taskPreset "-DBUILD_TESTING=$taskBuildTesting"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    if (-not $ConfigureOnly) {
        & $taskCMake --build --preset $taskPreset --parallel $Jobs
        if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
        if ($Test) {
            & (Join-Path (Split-Path $taskCMake) 'ctest.exe') --test-dir "build/$($Configuration.ToLowerInvariant())" --output-on-failure --no-tests=error
            if ($LASTEXITCODE -ne 0) { throw 'CTest failed.' }
        }
        $taskBin = Join-Path $taskRoot "build/$($Configuration.ToLowerInvariant())/bin"
        $taskProduct = [IO.File]::ReadAllText((Join-Path $taskBin 'product-info.json')) | ConvertFrom-Json
        Write-Output "Built: $(Join-Path $taskBin $taskProduct.runtimeExecutable)"
    }
} finally { Pop-Location }
