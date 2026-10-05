<#
.SYNOPSIS
  Configure and build ff7vr with MSVC x64 + Ninja (the CMake and Ninja bundled with Visual Studio).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\build.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\build.ps1 -BuildDir build\debug -Config Debug
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\build.ps1 -Without xr,xr_smoke -Target ff7vr

.NOTES
  Output: <BuildDir>\src\loader\xinput1_3.dll (+ .pdb and ff7vr.ini next to it). Exit code 0 on success.
  BuildDir defaults to build\<FF7VR_DEV_NAME env var, or 'dev'>; relative paths are relative to the repo root.
  -Without switches optional modules off (FF7VR_WITH_<NAME>=OFF): xr, xr_smoke, engine, render, dev.
  -With turns them back on. Both force a reconfigure; the choice then sticks in that build dir's cache.
#>
param(
    [string]$BuildDir = '',
    [ValidateSet('RelWithDebInfo', 'Debug', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [string]$Target = '',
    [string[]]$Without = @(),
    [string[]]$With = @(),
    [switch]$Clean,
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$BuildDir = Resolve-BuildDir $BuildDir

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Write-Step "Removing $BuildDir"
    Remove-Item -Recurse -Force -LiteralPath $BuildDir
}

$vs = Get-VsInstallPath
Enter-VsDevEnvironment $vs

$cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = Join-Path $cmakeDir 'CMake\bin\cmake.exe'
$ninja = Join-Path $cmakeDir 'Ninja\ninja.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw "Bundled cmake not found: $cmake (install the 'C++ CMake tools' VS component)" }

function ConvertTo-ModuleOptions([string[]]$names, [string]$value) {
    $out = @()
    foreach ($n in @($names | ForEach-Object { $_ -split ',' })) {
        $n = $n.Trim()
        if (-not $n) { continue }
        $leaf = ($n.Replace('\', '/') -split '/')[-1]
        $out += ('-DFF7VR_WITH_' + $leaf.ToUpperInvariant() + '=' + $value)
    }
    return $out
}
$moduleOpts = @(ConvertTo-ModuleOptions $Without 'OFF') + @(ConvertTo-ModuleOptions $With 'ON')

$needConfigure = $Reconfigure -or ($moduleOpts.Count -gt 0) -or -not (Test-Path -LiteralPath (Join-Path $BuildDir 'build.ninja'))
if ($needConfigure) {
    Write-Step "Configuring $BuildDir ($Config) $($moduleOpts -join ' ')"
    $cfgArgs = @('-S', $script:RepoRoot, '-B', $BuildDir, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja",
                 "-DCMAKE_BUILD_TYPE=$Config", '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl') + $moduleOpts
    & $cmake @cfgArgs
    if ($LASTEXITCODE -ne 0) { Write-Host "CONFIGURE FAILED ($LASTEXITCODE)"; exit $LASTEXITCODE }
}

Write-Step "Building $BuildDir"
$buildArgs = @('--build', $BuildDir)
if ($Target) { $buildArgs += @('--target', $Target) }
& $cmake @buildArgs
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED ($LASTEXITCODE)"; exit $LASTEXITCODE }

$dll = Join-Path $BuildDir 'src\loader\xinput1_3.dll'
if (Test-Path -LiteralPath $dll) { Write-Step "OK: $dll" } else { Write-Step 'OK' }
exit 0
