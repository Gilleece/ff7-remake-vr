# Shared helpers for the steamvr-*.ps1 scripts (headless OpenXR testing with
# SteamVR's null driver). Dot-source it: . "$PSScriptRoot\steamvr-common.ps1"
# Windows PowerShell 5.1 compatible. Nothing here is machine-specific: Steam and
# SteamVR are found through the registry, openvrpaths.vrpath and the Steam
# library list.

$script:SvRepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script:SvLockPath = Join-Path $script:SvRepoRoot '.locks\steamvr'
$script:SvBackupSuffix = '.ff7vr-backup'
$script:SvProcessNames = @('vrmonitor', 'vrserver', 'vrcompositor', 'vrdashboard', 'vrwebhelper', 'vrstartup',
                           'vrprismhost', 'vrserverhelper', 'vrservice', 'vrurlhandler')

function Write-Sv([string]$msg) { Write-Host "[steamvr] $msg" }

function Read-OpenVrPaths {
    $p = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    try { return (Get-Content -LiteralPath $p -Raw | ConvertFrom-Json) } catch { return $null }
}

function Get-SteamRoot {
    foreach ($k in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam')) {
        $props = Get-ItemProperty -Path $k -ErrorAction SilentlyContinue
        if (-not $props) { continue }
        foreach ($name in @('SteamPath', 'InstallPath')) {
            $v = $props.$name
            if ($v -and (Test-Path -LiteralPath $v)) { return ([System.IO.Path]::GetFullPath($v.Replace('/', '\'))) }
        }
    }
    return $null
}

function Get-SteamLibraryRoots {
    $roots = @()
    $steam = Get-SteamRoot
    if (-not $steam) { return $roots }
    $roots += $steam
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf) {
        foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $p = $m.Groups[1].Value.Replace('\\', '\')
            if ((Test-Path -LiteralPath $p) -and ($roots -notcontains $p)) { $roots += $p }
        }
    }
    return $roots
}

# SteamVR install folder (contains steamxr_win64.json).
function Get-SteamVrRoot {
    $ovr = Read-OpenVrPaths
    if ($ovr -and $ovr.runtime) {
        foreach ($r in @($ovr.runtime)) {
            if ($r -and (Test-Path -LiteralPath (Join-Path $r 'steamxr_win64.json'))) { return $r.TrimEnd('\', '/') }
        }
    }
    foreach ($lib in Get-SteamLibraryRoots) {
        $p = Join-Path $lib 'steamapps\common\SteamVR'
        if (Test-Path -LiteralPath (Join-Path $p 'steamxr_win64.json')) { return $p }
    }
    return $null
}

# Folder holding steamvr.vrsettings (normally <Steam>\config).
function Get-SteamVrConfigDir {
    $ovr = Read-OpenVrPaths
    if ($ovr -and $ovr.config) {
        foreach ($c in @($ovr.config)) {
            if ($c -and (Test-Path -LiteralPath $c)) { return ([System.IO.Path]::GetFullPath($c.Replace('/', '\'))).TrimEnd('\') }
        }
    }
    $steam = Get-SteamRoot
    if ($steam) { return (Join-Path $steam 'config') }
    return $null
}

function Get-VrSettingsPath {
    $dir = Get-SteamVrConfigDir
    if (-not $dir) { throw 'Steam config folder not found (is Steam installed?)' }
    return (Join-Path $dir 'steamvr.vrsettings')
}

function Get-VrSettingsBackupPath { return ((Get-VrSettingsPath) + $script:SvBackupSuffix) }

function Get-SteamVrProcesses {
    return @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $script:SvProcessNames -contains $_.ProcessName })
}

function Test-SteamVrRunning { return ((Get-SteamVrProcesses).Count -gt 0) }

# Stops every SteamVR process. vrmonitor is asked to close first so SteamVR can
# shut down cleanly; whatever is left after a few seconds is terminated.
function Stop-SteamVrProcesses([int]$timeoutSeconds = 15) {
    $procs = Get-SteamVrProcesses
    if ($procs.Count -eq 0) { return $true }
    Write-Sv "Stopping SteamVR ($(($procs | ForEach-Object { $_.ProcessName }) -join ', '))"
    foreach ($p in $procs) {
        if ($p.ProcessName -eq 'vrmonitor' -and $p.MainWindowHandle -ne [IntPtr]::Zero) { [void]$p.CloseMainWindow() }
    }
    $deadline = (Get-Date).AddSeconds([Math]::Min(5, $timeoutSeconds))
    while ((Get-Date) -lt $deadline -and (Test-SteamVrRunning)) { Start-Sleep -Milliseconds 300 }
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $left = Get-SteamVrProcesses
        if ($left.Count -eq 0) { break }
        foreach ($p in $left) { try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch { } }
        Start-Sleep -Milliseconds 500
    }
    $left = Get-SteamVrProcesses
    if ($left.Count -gt 0) {
        Write-Sv "Still running: $(($left | ForEach-Object { $_.ProcessName + ':' + $_.Id }) -join ', ')"
        return $false
    }
    Write-Sv 'SteamVR stopped'
    return $true
}

# ---------------------------------------------------------------- lock
# The directory .locks\steamvr exists while someone uses SteamVR from this repo.
# A lock is stale only if it is older than 20 minutes and SteamVR is not running.
function Get-SteamVrLockOwner {
    $f = Join-Path $script:SvLockPath 'owner.txt'
    if (-not (Test-Path -LiteralPath $f)) { return '' }
    return ((Get-Content -LiteralPath $f -Raw) -split '\s+')[0]
}

function Lock-SteamVr([string]$owner, [int]$waitSeconds = 0, [int]$pollSeconds = 30) {
    New-Item -ItemType Directory -Force -Path (Split-Path $script:SvLockPath -Parent) | Out-Null
    $deadline = (Get-Date).AddSeconds($waitSeconds)
    while ($true) {
        if (Test-Path -LiteralPath $script:SvLockPath) {
            $holder = Get-SteamVrLockOwner
            if ($holder -eq $owner) { Write-Sv "Lock already held by '$owner'"; return $true }
            $age = ((Get-Date) - (Get-Item -LiteralPath $script:SvLockPath).CreationTime).TotalMinutes
            if ($age -gt 20 -and -not (Test-SteamVrRunning)) {
                Write-Sv "Removing stale SteamVR lock held by '$holder' ($([int]$age) min old, SteamVR not running)"
                Remove-Item -Recurse -Force -LiteralPath $script:SvLockPath -ErrorAction SilentlyContinue
            }
        }
        $taken = $false
        try {
            New-Item -ItemType Directory -Path $script:SvLockPath -ErrorAction Stop | Out-Null
            $taken = $true
        } catch { }
        if ($taken) {
            $stamp = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
            Set-Content -LiteralPath (Join-Path $script:SvLockPath 'owner.txt') -Value "$owner $stamp pid=$PID" -Encoding ASCII
            Write-Sv "Lock taken by '$owner'"
            return $true
        }
        if ((Get-Date) -ge $deadline) {
            Write-Sv "SteamVR lock is held by '$(Get-SteamVrLockOwner)'"
            return $false
        }
        Write-Sv "SteamVR lock held by '$(Get-SteamVrLockOwner)'; waiting $pollSeconds s"
        Start-Sleep -Seconds $pollSeconds
    }
}

function Unlock-SteamVr([string]$owner, [switch]$force) {
    if (-not (Test-Path -LiteralPath $script:SvLockPath)) { return $true }
    $holder = Get-SteamVrLockOwner
    if (-not $force -and $holder -and $holder -ne $owner) {
        Write-Sv "Not releasing the SteamVR lock held by '$holder' (I am '$owner')"
        return $false
    }
    Remove-Item -Recurse -Force -LiteralPath $script:SvLockPath
    Write-Sv 'Lock released'
    return $true
}

function Get-DefaultSvOwner {
    if ($env:FF7VR_ROLE) { return $env:FF7VR_ROLE }
    return $env:USERNAME
}

# ---------------------------------------------------------------- JSON
function Write-JsonNoBom([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

# Sets $obj.$section.$key = $value on a PSCustomObject tree, creating the section if needed.
function Set-VrSetting($obj, [string]$section, [string]$key, $value) {
    if (-not ($obj.PSObject.Properties.Name -contains $section)) {
        $obj | Add-Member -NotePropertyName $section -NotePropertyValue ([pscustomobject]@{})
    }
    $sec = $obj.$section
    if ($sec.PSObject.Properties.Name -contains $key) { $sec.$key = $value }
    else { $sec | Add-Member -NotePropertyName $key -NotePropertyValue $value }
}
