$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'build\windows'
$vcvars = & 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Auxiliary\Build\vcvars64.bat'
$cases = @{
  wear_trigger = @('tests/wear_trigger.cpp','src/policy/wear_leveling_policy.cpp','src/policy/zone_directory.cpp')
  zone_popularity = @('tests/zone_popularity.cpp','src/policy/zone_directory.cpp')
  reserve_exchange = @('tests/reserve_exchange.cpp','src/policy/wear_leveling_policy.cpp','src/policy/zone_directory.cpp','src/policy/migration_executor.cpp','src/ssd/User_Request.cpp')
  advanced_swans = @('tests/advanced_swans.cpp','src/policy/wear_leveling_policy.cpp','src/policy/zone_directory.cpp','src/host/Trace_Pool.cpp')
}
$failed = @()
foreach ($name in $cases.Keys) {
  $sources = $cases[$name] | ForEach-Object { '"' + (Join-Path $root $_) + '"' }
  $exe = Join-Path $out "$name.exe"
  $batch = Join-Path $out "$name.cmd"
  Set-Content -LiteralPath $batch -Encoding ascii -Value @('@echo off', ('call "' + $vcvars + '" >nul'), ('cd /d "' + $out + '"'), ('cl /nologo /std:c++14 /EHsc /utf-8 /O2 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /Fe:"' + $exe + '" ' + ($sources -join ' ')), 'exit /b %errorlevel%')
  & $batch
  if ($LASTEXITCODE) { throw "Failed to build $name" }
  if ($name -eq 'advanced_swans') {
    $scratch = Join-Path $out ('pool-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $scratch | Out-Null
    & $exe $scratch
  } else { & $exe }
  if ($LASTEXITCODE) { $failed += $name }
}
if ($failed.Count) { throw ('Failed unit tests: ' + ($failed -join ', ')) }
