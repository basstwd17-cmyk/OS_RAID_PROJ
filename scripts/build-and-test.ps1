[CmdletBinding()]
param([switch]$RelinkOnly)
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $sourceRoot 'build'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'MSVC C++ tools are required.' }
& (Join-Path $vsPath 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
Push-Location $buildRoot
try {
    $sources = @(Get-ChildItem "$sourceRoot/src" -Recurse -Filter '*.cpp' | Where-Object Name -ne 'main.cpp' | ForEach-Object FullName)
    $flags = @('/nologo', '/EHsc', '/std:c++14', '/utf-8', '/Od', '/Zi', '/MD', '/D_CRT_SECURE_NO_WARNINGS', "/I$sourceRoot/src")
    if (-not $RelinkOnly) {
        & cl.exe @flags /MP4 /c @sources *> compile.log
        if ($LASTEXITCODE -ne 0) { throw 'Compilation failed; see build/compile.log.' }
    }
    $objects = @($sources | ForEach-Object { Join-Path $buildRoot ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj') })
    & cl.exe @flags "$sourceRoot/src/main.cpp" @objects "/Fe:$sourceRoot/MQSim.exe" /link /DEBUG *> link.log
    if ($LASTEXITCODE -ne 0) { throw 'Simulator link failed; see build/link.log.' }
    & cl.exe @flags "$sourceRoot/tests/migration_io_priority.cpp" @objects '/Fe:migration_io_priority.exe' /link /DEBUG *> test-build.log
    if ($LASTEXITCODE -ne 0) { throw 'Test build failed; see build/test-build.log.' }
    & ./migration_io_priority.exe
    if ($LASTEXITCODE -ne 0) { throw 'Migration I/O regression failed.' }
    Write-Output 'BUILD_OK: simulator and migration I/O regression tests'
} finally { Pop-Location }
