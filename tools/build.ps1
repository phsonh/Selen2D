[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [ValidateRange(1, 64)][int]$Jobs = 8,
    [ValidateRange(1, 4)][int]$DxcJobs = 4,
    [switch]$Test,
    [switch]$ConfigureOnly,
    [string]$LaunchConfig,
    [switch]$DefaultLaunch,
    [switch]$PublicRelease,
    [switch]$StageRelease,
    [string]$Python,
    [string]$ToolsetVersion
)
$ErrorActionPreference = 'Stop'
$OutputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = $OutputEncoding
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskPreset = if ($PublicRelease) { 'selen2d-public-release' } else { 'selen2d-' + $Configuration.ToLowerInvariant() }
$taskBuildDirectory = if ($PublicRelease) { 'build/public-release' } else { "build/$($Configuration.ToLowerInvariant())" }
if ($LaunchConfig -and $DefaultLaunch) { throw 'Choose -LaunchConfig or -DefaultLaunch, not both.' }
if ($PublicRelease -and ($Configuration -ne 'Release' -or $LaunchConfig)) {
    throw '-PublicRelease requires Release and the built-in default JSON; no custom -LaunchConfig.'
}
if ($StageRelease -and (-not $PublicRelease -or $ConfigureOnly)) {
    throw '-StageRelease requires -PublicRelease and an actual Release build.'
}
# Resolve on every invocation; a previous CMake cache is never a project selector.
$taskDefaultJson = Join-Path $taskRoot 'src/res/development/launch.json'
$taskPrivateJson = Join-Path $taskRoot 'launch/launch.json'
$taskJson = if ($PublicRelease -or $DefaultLaunch) { $taskDefaultJson }
    elseif ($LaunchConfig) { [IO.Path]::GetFullPath($LaunchConfig) }
    elseif (Test-Path -LiteralPath $taskPrivateJson -PathType Leaf) { $taskPrivateJson }
    else { $taskDefaultJson }
if (-not (Test-Path -LiteralPath $taskJson -PathType Leaf)) { throw "Launch JSON does not exist: $taskJson" }
Write-Output "Selected launch JSON: $taskJson"
if (-not $Python) {
    $taskVenvPython = Join-Path $taskRoot 'build/tools-venv/Scripts/python.exe'
    $Python = if (Test-Path -LiteralPath $taskVenvPython) { $taskVenvPython }
        else { (Get-Command python -ErrorAction Stop).Source }
}
$Python = (Get-Command $Python -ErrorAction Stop).Source
if ($Test -and -not (Test-Path -LiteralPath (Join-Path $taskRoot 'tests/CMakeLists.txt'))) {
    throw 'The -Test option requires local tests/CMakeLists.txt; tests are not included in this checkout.'
}
# Keep an existing Ninja tree's compiler and developer-shell libraries aligned.
# Fresh checkouts select the installed default; CI does not assume a minor version.
$taskCompilerRecords = @(Get-ChildItem -Path (Join-Path $taskRoot "$taskBuildDirectory/CMakeFiles/*/CMakeCXXCompiler.cmake") -ErrorAction SilentlyContinue)
if ($taskCompilerRecords.Count) {
    $taskRecord = $taskCompilerRecords | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $taskCompilerText = [IO.File]::ReadAllText($taskRecord.FullName)
    if ($taskCompilerText -match 'set\(CMAKE_CXX_COMPILER "([^"\r\n]+/MSVC/([0-9.]+)/[^"\r\n]+)"\)') {
        $taskCachedCompiler = $Matches[1]
        $taskCachedToolset = $Matches[2]
        if (-not (Test-Path -LiteralPath $taskCachedCompiler)) { throw 'The cached compiler is no longer installed; use a fresh build configuration. Existing caches were preserved.' }
        if ($ToolsetVersion -and -not ($taskCachedToolset -eq $ToolsetVersion -or $taskCachedToolset.StartsWith($ToolsetVersion + '.'))) {
            throw 'Requested toolset differs from the existing build configuration. Existing caches were preserved.'
        }
        $ToolsetVersion = $taskCachedToolset
        Write-Output "Reusing configured MSVC toolset: $ToolsetVersion"
    }
}
$taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs = & $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVs) { throw 'Visual Studio C++ tools are required.' }
Import-Module (Join-Path $taskVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
$taskDevArguments = '-arch=x64 -host_arch=x64'
if ($ToolsetVersion) { $taskDevArguments += " -vcvars_ver=$ToolsetVersion" }
Enter-VsDevShell -VsInstallPath $taskVs -SkipAutomaticLocation -DevCmdArguments $taskDevArguments
$taskCompiler = (Get-Command cl.exe -ErrorAction Stop).Source
Write-Output "Visual Studio: $taskVs"
Write-Output "MSVC: $taskCompiler ($((Get-Item -LiteralPath $taskCompiler).VersionInfo.FileVersion))"
Write-Output "Windows SDK: $env:WindowsSDKVersion"
$taskCMake = Join-Path $taskVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$taskNinja = Join-Path $taskVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja'
$env:PATH = "$taskNinja;$(Split-Path $taskCMake);$env:PATH"
Push-Location -LiteralPath $taskRoot
try {
    $taskBuildTesting = if ($Test) { 'ON' } else { 'OFF' }
    $taskPublic = if ($PublicRelease) { 'ON' } else { 'OFF' }
    $taskCMakeJson = if ($LaunchConfig -or $DefaultLaunch -or $PublicRelease) { $taskJson } else { '' }
    & $taskCMake --preset $taskPreset "-DBUILD_TESTING=$taskBuildTesting" "-DPython3_EXECUTABLE=$Python" `
        "-DENGINE_LAUNCH_CONFIG=$taskCMakeJson" '-DENGINE_LAUNCH_HEADER=' "-DENGINE_PUBLIC_RELEASE=$taskPublic" "-DENGINE_DXC_JOBS=$DxcJobs"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    if (-not $ConfigureOnly) {
        & $taskCMake --build --preset $taskPreset --parallel $Jobs
        if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
        if ($Test) {
            & (Join-Path (Split-Path $taskCMake) 'ctest.exe') --test-dir $taskBuildDirectory --output-on-failure --no-tests=error
            if ($LASTEXITCODE -ne 0) { throw 'CTest failed.' }
        }
        if ($StageRelease) {
            & $taskCMake --build $taskBuildDirectory --target stage_release
            if ($LASTEXITCODE -ne 0) { throw 'Public release staging failed.' }
        }
        $taskBin = Join-Path $taskRoot "$taskBuildDirectory/bin"
        $taskProduct = [IO.File]::ReadAllText((Join-Path $taskBin 'product-info.json')) | ConvertFrom-Json
        Write-Output "Built: $(Join-Path $taskBin $taskProduct.runtimeExecutable)"
    }
} finally { Pop-Location }
