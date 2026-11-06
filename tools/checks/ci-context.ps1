[CmdletBinding()]
param([string]$OutputDirectory = 'build/ci')
$ErrorActionPreference = 'Stop'
$taskWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVS = & $taskWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVS) { throw 'No compatible Visual Studio C++ installation.' }
Import-Module (Join-Path $taskVS 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $taskVS -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$taskCompiler = (Get-Command cl.exe -ErrorAction Stop).Source
$taskContext = [ordered]@{
    runnerImage = $env:ImageOS
    runnerImageVersion = $env:ImageVersion
    visualStudio = $taskVS
    compiler = $taskCompiler
    compilerVersion = (Get-Item -LiteralPath $taskCompiler).VersionInfo.FileVersion
    compilerSha256 = (Get-FileHash -LiteralPath $taskCompiler -Algorithm SHA256).Hash
    windowsSDK = $env:WindowsSDKVersion
    configuration = 'Release'
    dxcSourceCommit = '01b62ad47db7dee3dd33f90e7339cf9e860b3f93'
}
$taskText = $taskContext | ConvertTo-Json
$taskBytes = [Text.Encoding]::UTF8.GetBytes($taskText)
$taskHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($taskBytes)).ToLowerInvariant()
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
[IO.File]::WriteAllText((Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) 'toolchain.json'), $taskText)
Write-Output $taskText
if ($env:GITHUB_OUTPUT) { "identity=$taskHash" | Out-File -FilePath $env:GITHUB_OUTPUT -Encoding utf8 -Append }
