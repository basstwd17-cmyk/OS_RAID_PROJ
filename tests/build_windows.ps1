$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vcvars = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Auxiliary\Build\vcvars64.bat'
if (-not $vcvars) { throw 'Visual Studio C++ tools not found' }
$out = Join-Path $root 'build\windows'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$sources = Get-ChildItem -LiteralPath (Join-Path $root 'src') -Filter '*.cpp' -Recurse
$flags = '/nologo /std:c++14 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /O2 /MP'
$response = Join-Path $out 'sources.rsp'
$arguments = @($flags, ('/Fe:"' + (Join-Path $out 'MQSim.exe') + '"'))
$arguments += $sources | ForEach-Object { '"' + $_.FullName + '"' }
Set-Content -LiteralPath $response -Value $arguments -Encoding utf8
$batch = Join-Path $out 'compile.cmd'
Set-Content -LiteralPath $batch -Encoding ascii -Value @('@echo off', ('call "' + $vcvars + '" >nul'), ('cd /d "' + $out + '"'), ('cl @"' + $response + '"'), 'exit /b %errorlevel%')
& $batch
if ($LASTEXITCODE) { throw "C++ build failed: $LASTEXITCODE" }
