<#
.SYNOPSIS
  Builds a release of the mod and assembles a self-contained folder to play with it.

.DESCRIPTION
  1. Release build of the mod DLL (tools\dev\build.ps1 -Config Release, build\release).
  2. Assembles dist\ff7vr-<date>-<commit>\ with:
       xinput1_3.dll        the mod
       ff7vr.ini            the player's settings (from tools\package\ff7vr.ini)
       start-vr.cmd         starts a VR session (double-click)
       restore.cmd          puts the game folder back to normal (double-click)
       collect-diagnostics.cmd  zips the last session's log, the settings and system facts
       ff7vr-launcher.ps1   what the .cmd files run
       README.md            the user guide
       GUIDE.md             the detailed player guide (from docs\guide.md)
       LICENSE              the mod's licence
       THIRD-PARTY-NOTICES.md  the licences of the components built into the mod
       VERSION.txt          commit and build time
     and a .zip of that folder next to it.
  An existing folder of the same name is refreshed; its logs\ folder is kept.

  The package does not need this repository: copy the folder anywhere.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\package\package.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\package\package.ps1 -Clean
#>
param(
    [string]$BuildDir = 'build\release',
    [string]$OutDir = 'dist',
    [switch]$Clean,
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
function Step([string]$m) { Write-Host ("[{0}] {1}" -f (Get-Date).ToString('HH:mm:ss'), $m) }

if ($repo.Length -gt 60) {
    Step "WARNING: the repository path is $($repo.Length) characters long. Dependency builds can exceed"
    Step "         Windows' 260-character path limit ('Cannot open compiler generated file'); clone to a shorter path if so."
}

# ---- build
$buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $repo 'tools\dev\build.ps1'),
               '-BuildDir', $BuildDir, '-Config', 'Release', '-Target', 'ff7vr', '-Without', 'xr_smoke')
if ($Clean) { $buildArgs += '-Clean' }
Step "Building the mod (Release, $BuildDir)"
& powershell @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

$bd = $BuildDir
if (-not [System.IO.Path]::IsPathRooted($bd)) { $bd = Join-Path $repo $bd }
$dll = Join-Path $bd 'src\loader\xinput1_3.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "Build output missing: $dll" }

# ---- version
$commit = 'unknown'
$dirty = $false
try {
    $commit = (& git -C $repo rev-parse --short HEAD).Trim()
    $st = @(& git -C $repo status --porcelain --untracked-files=no)
    $dirty = ($st.Count -gt 0)
} catch { }
$date = (Get-Date).ToString('yyyyMMdd')
$name = "ff7vr-$date-$commit"
if ($dirty) {
    $name += '-modified'
    Step 'WARNING: the working tree has uncommitted changes; the package is marked "-modified".'
}

$od = $OutDir
if (-not [System.IO.Path]::IsPathRooted($od)) { $od = Join-Path $repo $od }
$pkg = Join-Path $od $name
New-Item -ItemType Directory -Force -Path $pkg | Out-Null

# ---- assemble (logs\ of an earlier copy stays)
$items = [ordered]@{
    'xinput1_3.dll'      = $dll
    'ff7vr.ini'          = (Join-Path $PSScriptRoot 'ff7vr.ini')
    'ff7vr-launcher.ps1' = (Join-Path $PSScriptRoot 'launcher\ff7vr-launcher.ps1')
    'start-vr.cmd'       = (Join-Path $PSScriptRoot 'launcher\start-vr.cmd')
    'restore.cmd'        = (Join-Path $PSScriptRoot 'launcher\restore.cmd')
    'collect-diagnostics.cmd' = (Join-Path $PSScriptRoot 'launcher\collect-diagnostics.cmd')
    'README.md'          = (Join-Path $repo 'README.md')
    'GUIDE.md'           = (Join-Path $repo 'docs\guide.md')
    'LICENSE'            = (Join-Path $repo 'LICENSE')
    'THIRD-PARTY-NOTICES.md' = (Join-Path $repo 'THIRD-PARTY-NOTICES.md')
}
foreach ($k in $items.Keys) {
    if (-not (Test-Path -LiteralPath $items[$k])) { throw "Missing $($items[$k])" }
    Copy-Item -LiteralPath $items[$k] -Destination (Join-Path $pkg $k) -Force
}
$ver = @(
    "ff7vr $name",
    "commit:  $commit$(if ($dirty) { ' (with uncommitted changes)' })",
    "built:   $((Get-Item -LiteralPath $dll).LastWriteTime.ToString('yyyy-MM-dd HH:mm'))",
    "dll sha256: $((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash)"
)
[System.IO.File]::WriteAllLines((Join-Path $pkg 'VERSION.txt'), [string[]]$ver)

if (-not $NoZip) {
    $zip = "$pkg.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    $files = @(Get-ChildItem -LiteralPath $pkg -File | ForEach-Object { $_.FullName })
    Compress-Archive -LiteralPath $files -DestinationPath $zip
    Step "Zip: $zip"
}

Step "Package ready: $pkg"
Get-ChildItem -LiteralPath $pkg | ForEach-Object { Write-Host ("    {0,-24} {1,10:N0} bytes" -f $_.Name, $(if ($_.PSIsContainer) { 0 } else { $_.Length })) }
Write-Host ''
Write-Host "Start a session:  $pkg\start-vr.cmd"
Write-Host "Back to normal:   $pkg\restore.cmd"
Write-Host "Diagnostics:      $pkg\collect-diagnostics.cmd"
