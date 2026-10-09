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
       VERSION.txt          version (git describe), commit, build time, SHA-256 of the DLL
     and a .zip of that folder next to it.
  3. A drop-in zip next to it, ff7vr-<date>-<commit>-dropin.zip, for installing
     the mod by hand without the launcher. It holds only what goes into the
     game's End\Binaries\Win64 folder:
       xinput1_3.dll, ff7vr.ini, ff7vr-start.cmd (starts the game directly)
       ff7vr-docs\README.md, GUIDE.md, LICENSE, THIRD-PARTY-NOTICES.md, VERSION.txt
     Installing = unzipping it into End\Binaries\Win64; see GUIDE.md,
     "Installing without the launcher".
  4. dist\<package>-SHA256SUMS.txt (the zips' SHA-256, "<hash>  <file>") and
     dist\<package>-release-notes-snippet.md (a "Verify your download" section for the
     release notes). See docs\releasing.md.
  An existing folder of the same name is refreshed; its logs\ folder is kept.

  The package does not need this repository: copy the folder anywhere.

  -Dlss builds the mod with NVIDIA DLSS (-DFF7VR_DLSS=ON, docs\dlss.md; the NVIDIA DLSS
  SDK is fetched at build time and is not part of the package) in build\release-dlss and
  names the package ff7vr-<date>-<commit>-dlss. Without it the build is explicitly
  without DLSS.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\package\package.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\package\package.ps1 -Clean
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\package\package.ps1 -Dlss
#>
param(
    [string]$BuildDir = '',
    [string]$OutDir = 'dist',
    [switch]$Clean,
    [switch]$NoZip,
    [switch]$Dlss
)
if (-not $BuildDir) { $BuildDir = if ($Dlss) { 'build\release-dlss' } else { 'build\release' } }

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
function Step([string]$m) { Write-Host ("[{0}] {1}" -f (Get-Date).ToString('HH:mm:ss'), $m) }

if ($repo.Length -gt 60) {
    Step "WARNING: the repository path is $($repo.Length) characters long. Dependency builds can exceed"
    Step "         Windows' 260-character path limit ('Cannot open compiler generated file'); clone to a shorter path if so."
}

# ---- build
$buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $repo 'tools\dev\build.ps1'),
               '-BuildDir', $BuildDir, '-Config', 'Release', '-Target', 'ff7vr')
# DLSS is a cached CMake option of the build folder: state it on every build.
if ($Dlss) { $buildArgs += @('-Without', 'xr_smoke', '-With', 'dlss') } else { $buildArgs += @('-Without', 'xr_smoke,dlss') }
if ($Clean) { $buildArgs += '-Clean' }
Step "Building the mod (Release, $BuildDir$(if ($Dlss) { ', with DLSS' }))"
& powershell @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

$bd = $BuildDir
if (-not [System.IO.Path]::IsPathRooted($bd)) { $bd = Join-Path $repo $bd }
$dll = Join-Path $bd 'src\loader\xinput1_3.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "Build output missing: $dll" }
# Whether DLSS is in the DLL is checked on the DLL itself: NGX's initialisation entry point is named only by
# the DLSS build (every build names NGX's create and evaluate entry points, which it hooks for Luma).
$hasNgx = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($dll)).Contains('NVSDK_NGX_D3D11_Init')
if ($Dlss -and -not $hasNgx) { throw 'Built without DLSS although -Dlss was given (is the NVIDIA DLSS SDK there? see the CMake output)' }
if (-not $Dlss -and $hasNgx) { throw 'The DLL contains DLSS although -Dlss was not given' }

# ---- version
$commit = 'unknown'
$dirty = $false
try {
    $commit = (& git -C $repo rev-parse --short HEAD).Trim()
    $st = @(& git -C $repo status --porcelain --untracked-files=no)
    $dirty = ($st.Count -gt 0)
} catch { }
# The version the DLL was built with (generated header), else git describe, else 'unknown'.
$version = 'unknown'
$hdr = Join-Path $bd 'generated\ff7vr_buildinfo.h'
$m = $null
if (Test-Path -LiteralPath $hdr) { $m = Select-String -LiteralPath $hdr -Pattern '#define FF7VR_VERSION "([^"]*)"' | Select-Object -First 1 }
if ($m) { $version = $m.Matches[0].Groups[1].Value }
else { try { $d = (& git -C $repo describe --tags --always --dirty 2>$null); if ($LASTEXITCODE -eq 0 -and $d) { $version = "$d".Trim() } } catch { } }
$date = (Get-Date).ToString('yyyyMMdd')
$name = "ff7vr-$date-$commit"
if ($dirty) {
    $name += '-modified'
    Step 'WARNING: the working tree has uncommitted changes; the package is marked "-modified".'
}
if ($Dlss) { $name += '-dlss' }

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
if ($Dlss) {
    # The -dlss package ships with DLSS on at input_scale 0.65 (the template keeps it off:
    # the standard build has no DLSS).
    $iniPath = Join-Path $pkg 'ff7vr.ini'
    $ini = [System.IO.File]::ReadAllText($iniPath)
    $ini = $ini -replace '(?m)^(\[dlss\](?:\r?\n(?!\[)[^\r\n]*)*?\r?\nenabled = )0 ', '${1}1 '
    $ini = $ini -replace '(?m)^(\[dlss\](?:\r?\n(?!\[)[^\r\n]*)*?\r?\ninput_scale = )0\.5 ', '${1}0.65'
    if ($ini -notmatch '(?m)^\[dlss\](?:\r?\n(?!\[)[^\r\n]*)*?\r?\nenabled = 1 ') { throw 'Could not switch DLSS on in the package ini' }
    if ($ini -notmatch '(?m)^\[dlss\](?:\r?\n(?!\[)[^\r\n]*)*?\r?\ninput_scale = 0\.65') { throw 'Could not set input_scale = 0.65 in the package ini' }
    [System.IO.File]::WriteAllText($iniPath, $ini)
}
$ver = @(
    "ff7vr $version ($commit)",
    "package: $name",
    "commit:  $commit$(if ($dirty) { ' (with uncommitted changes)' })",
    "built:   $((Get-Item -LiteralPath $dll).LastWriteTime.ToString('yyyy-MM-dd HH:mm'))",
    "dll sha256: $((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash)",
    "dlss:    $(if ($Dlss) { 'built in (NVIDIA DLSS SDK; the DLSS model nvngx_dlss.dll is not included: the player puts NVIDIA''s copy beside the game''s exe, see README.md)' } else { 'not built in' })"
)
[System.IO.File]::WriteAllLines((Join-Path $pkg 'VERSION.txt'), [string[]]$ver)

if (-not $NoZip) {
    $zip = "$pkg.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    $files = @(Get-ChildItem -LiteralPath $pkg -File | ForEach-Object { $_.FullName })
    Compress-Archive -LiteralPath $files -DestinationPath $zip
    Step "Zip: $zip"

    # Drop-in zip: entries written one by one with '/' separators (Compress-Archive
    # in Windows PowerShell 5.1 writes backslashes into the names of files in sub-folders).
    $dropin = "$pkg-dropin.zip"
    if (Test-Path -LiteralPath $dropin) { Remove-Item -LiteralPath $dropin -Force }
    $entries = [ordered]@{
        'xinput1_3.dll'                    = (Join-Path $pkg 'xinput1_3.dll')
        'ff7vr.ini'                        = (Join-Path $pkg 'ff7vr.ini')
        'ff7vr-start.cmd'                  = (Join-Path $PSScriptRoot 'dropin\ff7vr-start.cmd')
        'ff7vr-docs/README.md'             = (Join-Path $pkg 'README.md')
        'ff7vr-docs/GUIDE.md'              = (Join-Path $pkg 'GUIDE.md')
        'ff7vr-docs/LICENSE'               = (Join-Path $pkg 'LICENSE')
        'ff7vr-docs/THIRD-PARTY-NOTICES.md' = (Join-Path $pkg 'THIRD-PARTY-NOTICES.md')
        'ff7vr-docs/VERSION.txt'           = (Join-Path $pkg 'VERSION.txt')
    }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $za = [System.IO.Compression.ZipFile]::Open($dropin, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($k in $entries.Keys) {
            [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $entries[$k], $k,
                [System.IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $za.Dispose() }
    Step "Drop-in zip: $dropin"

    # Hashes of the zips, for the release notes and for players to check their download.
    $sums = @()
    foreach ($z in @($zip, $dropin)) {
        $sums += ('{0}  {1}' -f (Get-FileHash -LiteralPath $z -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path -Leaf $z))
    }
    $sumsFile = Join-Path $od "$name-SHA256SUMS.txt"
    [System.IO.File]::WriteAllLines($sumsFile, [string[]]$sums)
    Step "SHA-256: $sumsFile"

    $ghRepo = $env:GITHUB_REPOSITORY
    if (-not $ghRepo) {
        try {
            $url = (& git -C $repo remote get-url origin 2>$null)
            if ($LASTEXITCODE -eq 0 -and "$url" -match 'github\.com[:/]([^/]+/[^/]+?)(\.git)?$') { $ghRepo = $Matches[1] }
        } catch { }
    }
    $notes = @('## Verify your download', '',
               'SHA-256 of each file of this release:', '', '```')
    $notes += $sums
    $notes += @('```', '',
                'To check a download on Windows, run this in a command prompt in the folder of the zip and compare',
                'the result with the line above (upper or lower case does not matter):', '',
                '```', "certutil -hashfile $(Split-Path -Leaf $zip) SHA256", '```')
    if ($ghRepo) {
        $notes += @('', 'Zips built by the release workflow also carry a signed build provenance attestation; with the',
                    'GitHub CLI:', '', '```', "gh attestation verify $(Split-Path -Leaf $zip) --repo $ghRepo", '```')
    }
    $notesFile = Join-Path $od "$name-release-notes-snippet.md"
    [System.IO.File]::WriteAllLines($notesFile, [string[]]$notes)
    Step "Release notes snippet: $notesFile"
}

Step "Package ready: $pkg"
Get-ChildItem -LiteralPath $pkg | ForEach-Object { Write-Host ("    {0,-24} {1,10:N0} bytes" -f $_.Name, $(if ($_.PSIsContainer) { 0 } else { $_.Length })) }
Write-Host ''
Write-Host "Start a session:  $pkg\start-vr.cmd"
Write-Host "Back to normal:   $pkg\restore.cmd"
Write-Host "Diagnostics:      $pkg\collect-diagnostics.cmd"
if (-not $NoZip) { Write-Host "Without launcher: unzip $name-dropin.zip into the game's End\Binaries\Win64 (GUIDE.md)" }
