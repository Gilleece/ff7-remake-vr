# Shared helpers for the tools/dev scripts. Dot-source it:  . "$PSScriptRoot\common.ps1"
# Windows PowerShell 5.1 compatible (no &&, ||, ternary or null-coalescing).
#
# Nothing machine-specific is hardcoded. Paths are detected, and each can be
# overridden with an environment variable:
#   FF7VR_GAME_DIR    game install root (folder containing End\ and Engine\)
#   FF7VR_SAVE_ROOT   "Documents\My Games\FINAL FANTASY VII REMAKE"
#   FF7VR_BACKUP_DIR  where save backups go (default %USERPROFILE%\ff7-remake-vr-backups)
#   FF7VR_VS_PATH     Visual Studio installation folder
#   FF7VR_DEV_NAME    your name for this checkout: default build directory
#                     (build\<name>) and owner name written into the game lock
#                     (default 'dev')

Set-StrictMode -Version 2.0

$script:RepoRoot        = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script:SteamAppId      = 1462040
$script:GameProcessName = 'ff7remake_'
$script:LockDir         = Join-Path $script:RepoRoot '.locks'
$script:GameLock        = Join-Path $script:LockDir 'game'
$script:StateDir        = Join-Path $script:LockDir 'state'      # .locks/ is gitignored
$script:LumaStatePath   = Join-Path $script:StateDir 'luma-disabled.txt'
$script:CapturesDir     = Join-Path $script:RepoRoot 'captures'  # gitignored
$script:LogName         = 'ff7vr.log'
$script:ProxyDllName    = 'xinput1_3.dll'
$script:ManifestName    = 'ff7vr.deploy-manifest.json'           # lives in the game's Win64 folder
$script:CachedGameRoot  = $null

function Write-Step([string]$msg) {
    $t = (Get-Date).ToString('HH:mm:ss')
    Write-Host "[$t] $msg"
}

function Ensure-Dir([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { New-Item -ItemType Directory -Force -Path $path | Out-Null }
}

function Get-DevName {
    if ($env:FF7VR_DEV_NAME) { return $env:FF7VR_DEV_NAME }
    return 'dev'
}

function Get-DefaultBuildDir {
    return (Join-Path $script:RepoRoot ('build\' + (Get-DevName)))
}

# Resolves a build directory argument (relative to the repo root) or the default.
function Resolve-BuildDir([string]$buildDir) {
    if (-not $buildDir) { return (Get-DefaultBuildDir) }
    if ([System.IO.Path]::IsPathRooted($buildDir)) { return $buildDir }
    return (Join-Path $script:RepoRoot $buildDir)
}

# ------------------------------------------------------------------ toolchain

function Get-VsInstallPath {
    if ($env:FF7VR_VS_PATH) { return $env:FF7VR_VS_PATH }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere.exe not found at $vswhere; set FF7VR_VS_PATH" }
    $path = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $path) { throw 'No Visual Studio with the C++ x64 tools found; set FF7VR_VS_PATH' }
    return ($path | Select-Object -First 1)
}

function Enter-VsDevEnvironment([string]$vsPath) {
    if ($env:VSCMD_VER -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) { return }
    $launcher = Join-Path $vsPath 'Common7\Tools\Launch-VsDevShell.ps1'
    if (-not (Test-Path -LiteralPath $launcher)) { throw "Launch-VsDevShell.ps1 not found under $vsPath" }
    # The dev shell calls vswhere by name; make sure it is on PATH.
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    if ($env:PATH -notlike "*$installer*") { $env:PATH = "$env:PATH;$installer" }
    Write-Step "Entering VS developer environment (x64): $vsPath"
    $here = Get-Location
    & $launcher -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    Set-Location $here
}

# ------------------------------------------------------------------ game paths

# Steam library folders from libraryfolders.vdf (simple "path" key scan).
function Get-SteamLibraries {
    $libs = @()
    $steam = $null
    try { $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath } catch { }
    if (-not $steam) { return $libs }
    $steam = $steam.Replace('/', '\')
    $libs += $steam
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf) {
        foreach ($line in Get-Content -LiteralPath $vdf) {
            if ($line -match '^\s*"path"\s*"(.+)"\s*$') { $libs += $Matches[1].Replace('\\', '\') }
        }
    }
    return @($libs | Select-Object -Unique)
}

function Get-SteamExe {
    try {
        $p = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamExe -ErrorAction Stop).SteamExe
        if ($p) { return $p.Replace('/', '\') }
    } catch { }
    return $null
}

function Get-GameRoot {
    if ($script:CachedGameRoot) { return $script:CachedGameRoot }
    $root = $null
    if ($env:FF7VR_GAME_DIR) {
        $root = $env:FF7VR_GAME_DIR
    } else {
        foreach ($lib in Get-SteamLibraries) {
            $acf = Join-Path $lib ("steamapps\appmanifest_{0}.acf" -f $script:SteamAppId)
            if (-not (Test-Path -LiteralPath $acf)) { continue }
            $installdir = $null
            foreach ($line in Get-Content -LiteralPath $acf) {
                if ($line -match '^\s*"installdir"\s*"(.+)"\s*$') { $installdir = $Matches[1] }
            }
            if ($installdir) {
                $cand = Join-Path $lib ("steamapps\common\" + $installdir)
                if (Test-Path -LiteralPath $cand) { $root = $cand; break }
            }
        }
    }
    if (-not $root -or -not (Test-Path -LiteralPath (Join-Path $root 'End\Binaries\Win64\ff7remake_.exe'))) {
        throw "Game install not found (Steam app $script:SteamAppId). Set FF7VR_GAME_DIR to the folder containing End\ and Engine\."
    }
    $script:CachedGameRoot = $root
    return $root
}

function Get-GameBinDir { return (Join-Path (Get-GameRoot) 'End\Binaries\Win64') }
function Get-GameExe    { return (Join-Path (Get-GameBinDir) 'ff7remake_.exe') }
function Get-LumaDll    { return (Join-Path (Get-GameBinDir) 'dxgi.dll') }
function Get-LumaDisabledDll { return (Join-Path (Get-GameBinDir) 'dxgi.dll.vr-disabled') }

# "Documents\My Games\FINAL FANTASY VII REMAKE" (the game's whole user-data folder).
function Get-SaveRoot {
    if ($env:FF7VR_SAVE_ROOT) { return $env:FF7VR_SAVE_ROOT }
    return (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\FINAL FANTASY VII REMAKE')
}

# The per-Steam-account save folder(s) under Steam\ that contain .sav files.
function Get-SaveDirs {
    $steamDir = Join-Path (Get-SaveRoot) 'Steam'
    if (-not (Test-Path -LiteralPath $steamDir)) { return @() }
    return @(Get-ChildItem -LiteralPath $steamDir -Directory |
             Where-Object { @(Get-ChildItem -LiteralPath $_.FullName -Filter '*.sav' -File -ErrorAction SilentlyContinue).Count -gt 0 } |
             ForEach-Object { $_.FullName })
}

function Get-BackupRoot {
    if ($env:FF7VR_BACKUP_DIR) { return $env:FF7VR_BACKUP_DIR }
    return (Join-Path $env:USERPROFILE 'ff7-remake-vr-backups')
}

# ------------------------------------------------------------------ processes

function Get-GameProcesses {
    return @(Get-Process -Name $script:GameProcessName -ErrorAction SilentlyContinue)
}

# Helper processes a game run can leave behind (UE crash reporter, the launcher stub).
function Get-GameHelperProcesses {
    $out = @()
    foreach ($n in @('CrashReportClient', 'ff7remake')) {
        foreach ($p in @(Get-Process -Name $n -ErrorAction SilentlyContinue)) {
            # Only processes started from the game folder (never unrelated programs with the same name).
            $path = $null
            try { $path = $p.Path } catch { }
            if ($path -and $path.StartsWith((Get-GameRoot), [System.StringComparison]::OrdinalIgnoreCase)) { $out += $p }
        }
    }
    return $out
}

function Get-FileSha256([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

function Get-GameLogPath { return (Join-Path (Get-GameBinDir) $script:LogName) }

# ------------------------------------------------------------------ Win32 helpers
if (-not ('FF7VR.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
namespace FF7VR {
public static class Native {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder sb, int max);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr w, IntPtr l);

    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public INPUTUNION u; }
    [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);

    public static IntPtr[] WindowsOfProcess(uint pid, bool visibleOnly) {
        var list = new System.Collections.Generic.List<IntPtr>();
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p == pid && (!visibleOnly || IsWindowVisible(h))) list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern uint GetLongPathName(string shortPath, StringBuilder sb, uint max);
    // Expands 8.3 short names (PROGRA~1 style) so paths compare reliably.
    public static string LongPath(string p) { var sb = new StringBuilder(32768); uint n = GetLongPathName(p, sb, 32768); return (n > 0 && n < 32768) ? sb.ToString() : p; }
    public static string Title(IntPtr h) { var sb = new StringBuilder(512); GetWindowText(h, sb, 512); return sb.ToString(); }
    public static string ClassOf(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }

    // Key press via SendInput with scan codes (games usually read scan codes). Goes to the foreground window.
    public static uint KeyTap(ushort vk, int holdMs, bool extended) {
        ushort scan = (ushort)MapVirtualKey(vk, 0);
        uint flags = 0x0008; // KEYEVENTF_SCANCODE
        if (extended) flags |= 0x0001;
        var down = new INPUT[1]; down[0].type = 1; down[0].u.ki.wScan = scan; down[0].u.ki.dwFlags = flags;
        var up = new INPUT[1]; up[0].type = 1; up[0].u.ki.wScan = scan; up[0].u.ki.dwFlags = flags | 0x0002;
        uint a = SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(holdMs);
        uint b = SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
        return a + b;
    }
}
}
'@
}

# The game's main window (largest visible top-level window of ff7remake_.exe), or [IntPtr]::Zero.
function Get-GameWindow {
    foreach ($p in Get-GameProcesses) {
        $best = [IntPtr]::Zero
        $bestArea = 0
        foreach ($h in [FF7VR.Native]::WindowsOfProcess([uint32]$p.Id, $true)) {
            $r = New-Object FF7VR.Native+RECT
            [void][FF7VR.Native]::GetWindowRect($h, [ref]$r)
            $area = ($r.Right - $r.Left) * ($r.Bottom - $r.Top)
            if ($area -gt $bestArea) { $best = $h; $bestArea = $area }
        }
        if ($best -ne [IntPtr]::Zero -and $bestArea -gt 10000) { return $best }
    }
    return [IntPtr]::Zero
}

# Brings the game window to the foreground (needed for keyboard input and for
# screen-copy capture). Returns $true if it is the foreground window afterwards.
function Set-GameForeground([IntPtr]$hwnd) {
    if ($hwnd -eq [IntPtr]::Zero) { return $false }
    if ([FF7VR.Native]::IsIconic($hwnd)) { [void][FF7VR.Native]::ShowWindow($hwnd, 9) }  # SW_RESTORE
    if ([FF7VR.Native]::GetForegroundWindow() -eq $hwnd) { return $true }
    # Foreground-lock workaround: a synthetic ALT tap allows SetForegroundWindow from a background process.
    [FF7VR.Native]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
    [FF7VR.Native]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    [void][FF7VR.Native]::SetForegroundWindow($hwnd)
    [void][FF7VR.Native]::BringWindowToTop($hwnd)
    Start-Sleep -Milliseconds 200
    return ([FF7VR.Native]::GetForegroundWindow() -eq $hwnd)
}

# ------------------------------------------------------------------ game lock
# Protocol: the directory .locks\game exists while someone runs the game.
# Creating a directory is atomic, so New-Item doubles as test-and-set.
# owner.txt inside holds "<owner> <ISO time> pid=<powershell pid>".
# A lock is stale only if it is older than 20 minutes AND no ff7remake_ process runs.

function Get-DefaultOwner { return (Get-DevName) }

function Get-LockInfo([string]$lockPath = $script:GameLock) {
    if (-not (Test-Path -LiteralPath $lockPath)) { return $null }
    $ownerFile = Join-Path $lockPath 'owner.txt'
    $text = ''
    if (Test-Path -LiteralPath $ownerFile) { $text = (Get-Content -LiteralPath $ownerFile -Raw -ErrorAction SilentlyContinue) }
    if ($null -eq $text) { $text = '' }
    $owner = ($text.Trim() -split '\s+')[0]
    $created = (Get-Item -LiteralPath $lockPath).CreationTime
    return [pscustomobject]@{ Path = $lockPath; Owner = $owner; Text = $text.Trim(); Created = $created;
                              AgeMinutes = [math]::Round(((Get-Date) - $created).TotalMinutes, 1) }
}

# Returns $true once the lock is ours. Waits (polling every PollSeconds) up to WaitSeconds.
function Lock-Game([string]$owner = (Get-DefaultOwner), [int]$waitSeconds = 0, [int]$pollSeconds = 30,
                   [string]$lockPath = $script:GameLock) {
    Ensure-Dir (Split-Path $lockPath -Parent)
    $deadline = (Get-Date).AddSeconds($waitSeconds)
    while ($true) {
        $info = Get-LockInfo $lockPath
        if ($info -and $info.Owner -eq $owner) {
            Write-Step "Lock $lockPath already held by '$owner' (re-entrant)"
            return $true
        }
        if ($info -and $info.AgeMinutes -gt 20 -and @(Get-GameProcesses).Count -eq 0) {
            Write-Step "Removing stale lock held by '$($info.Owner)' ($($info.AgeMinutes) min old, game not running)"
            Remove-Item -Recurse -Force -LiteralPath $lockPath -ErrorAction SilentlyContinue
        }
        $ok = $false
        try {
            New-Item -ItemType Directory -Path $lockPath -ErrorAction Stop | Out-Null
            $ok = $true
        } catch { $ok = $false }
        if ($ok) {
            $stamp = (Get-Date).ToString('s')
            Set-Content -LiteralPath (Join-Path $lockPath 'owner.txt') -Value "$owner $stamp pid=$PID" -Encoding ASCII
            Write-Step "Lock $lockPath taken by '$owner'"
            return $true
        }
        $info = Get-LockInfo $lockPath
        if ((Get-Date) -ge $deadline) {
            if ($info) { Write-Step "Lock busy: held by '$($info.Owner)' for $($info.AgeMinutes) min ($($info.Text))" }
            return $false
        }
        if ($info) { Write-Step "Lock held by '$($info.Owner)' ($($info.AgeMinutes) min); waiting $pollSeconds s" }
        Start-Sleep -Seconds $pollSeconds
    }
}

# Releases the lock if held by $owner (or by anyone with -Force). Returns $true if no lock remains.
function Unlock-Game([string]$owner = (Get-DefaultOwner), [switch]$force, [string]$lockPath = $script:GameLock) {
    $info = Get-LockInfo $lockPath
    if (-not $info) { return $true }
    if ($info.Owner -ne $owner -and -not $force) {
        Write-Step "Not releasing lock held by '$($info.Owner)' (I am '$owner')"
        return $false
    }
    Remove-Item -Recurse -Force -LiteralPath $lockPath
    Write-Step "Lock $lockPath released"
    return $true
}

# ------------------------------------------------------------------ Luma / ReShade dxgi.dll

# Renames dxgi.dll -> dxgi.dll.vr-disabled. Returns a status string.
function Disable-Luma {
    $dll = Get-LumaDll
    $off = Get-LumaDisabledDll
    $hasOn = Test-Path -LiteralPath $dll
    $hasOff = Test-Path -LiteralPath $off
    if ($hasOn -and $hasOff) { throw "Both $dll and $off exist; refusing to touch either. Resolve by hand." }
    if ($hasOff) { return 'already-disabled' }
    if (-not $hasOn) { return 'not-installed' }
    Ensure-Dir $script:StateDir
    Rename-Item -LiteralPath $dll -NewName 'dxgi.dll.vr-disabled'
    Set-Content -LiteralPath $script:LumaStatePath -Value ((Get-Date).ToString('s')) -Encoding ASCII
    return 'disabled'
}

# Renames dxgi.dll.vr-disabled back to dxgi.dll. Idempotent.
function Restore-Luma {
    $dll = Get-LumaDll
    $off = Get-LumaDisabledDll
    $hasOn = Test-Path -LiteralPath $dll
    $hasOff = Test-Path -LiteralPath $off
    if ($hasOff -and -not $hasOn) {
        $tries = 0
        while ($true) {
            try { Rename-Item -LiteralPath $off -NewName 'dxgi.dll' -ErrorAction Stop; break }
            catch {
                $tries++
                if ($tries -ge 20) { throw "Could not rename $off back to dxgi.dll: $_" }
                Start-Sleep -Milliseconds 500
            }
        }
        Remove-Item -LiteralPath $script:LumaStatePath -ErrorAction SilentlyContinue
        return 'restored'
    }
    if ($hasOff -and $hasOn) { return 'conflict: both dxgi.dll and dxgi.dll.vr-disabled exist (left untouched)' }
    Remove-Item -LiteralPath $script:LumaStatePath -ErrorAction SilentlyContinue
    if ($hasOn) { return 'already-enabled' }
    return 'not-installed'
}

# ------------------------------------------------------------------ waiting helpers

# Waits until $condition returns a truthy value or the timeout expires. Returns the value or $null.
function Wait-Until([scriptblock]$condition, [int]$timeoutSeconds, [int]$pollMs = 500, [string]$what = 'condition') {
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $v = & $condition
        if ($v) { return $v }
        Start-Sleep -Milliseconds $pollMs
    }
    Write-Step "Timed out after $timeoutSeconds s waiting for $what"
    return $null
}

# True if the file contains a line matching $pattern (regex). Reads with shared access.
function Test-LogContains([string]$path, [string]$pattern) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    try {
        $share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
        $fs = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $share)
        $sr = New-Object System.IO.StreamReader($fs)
        $text = $sr.ReadToEnd()
        $sr.Close()
        return ($text -match $pattern)
    } catch { return $false }
}

# Reads a file that another process may hold open for writing (the game's log).
function Read-SharedText([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    try {
        $share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
        $fs = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $share)
        $sr = New-Object System.IO.StreamReader($fs)
        $text = $sr.ReadToEnd()
        $sr.Close()
        return $text
    } catch { return '' }
}

# ------------------------------------------------------------------ save backup
# A backup is a folder <BackupRoot>\saves-<yyyyMMdd-HHmmss>\ holding a copy of
# the save root's Steam\ folder (every account folder) and Saved\Config\, plus
# backup-manifest.json listing every file with its size and SHA-256. The
# manifest is written last, only after every copied file was verified against
# its source, so a folder without it is an incomplete backup and is ignored.

$script:BackupScopes = @('Steam', 'Saved\Config')

function Get-RelativePath([string]$base, [string]$full) {
    $full = [FF7VR.Native]::LongPath($full)
    $b = [FF7VR.Native]::LongPath($base).TrimEnd('\') + '\'
    if ($full.StartsWith($b, [System.StringComparison]::OrdinalIgnoreCase)) { return $full.Substring($b.Length) }
    throw "$full is not under $base"
}

function Get-ScopeFiles([string]$root) {
    $out = @()
    foreach ($scope in $script:BackupScopes) {
        $dir = Join-Path $root $scope
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        foreach ($f in @(Get-ChildItem -LiteralPath $dir -Recurse -File -Force)) { $out += $f }
    }
    return $out
}

function Read-BackupManifest([string]$backupDir) {
    $m = Join-Path $backupDir 'backup-manifest.json'
    if (-not (Test-Path -LiteralPath $m)) { return $null }
    try { return (Get-Content -LiteralPath $m -Raw | ConvertFrom-Json) } catch { return $null }
}

# Complete backups (with a manifest), newest first.
function Get-SaveBackups([string]$backupRoot = (Get-BackupRoot)) {
    if (-not (Test-Path -LiteralPath $backupRoot)) { return @() }
    return @(Get-ChildItem -LiteralPath $backupRoot -Directory -Filter 'saves-*' |
             Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'backup-manifest.json') } |
             Sort-Object Name -Descending | ForEach-Object { $_.FullName })
}

# Re-hashes every file of a backup against its manifest. Returns a list of problems (empty = good).
function Test-SaveBackup([string]$backupDir) {
    $problems = @()
    $m = Read-BackupManifest $backupDir
    if (-not $m) { return @("no readable backup-manifest.json in $backupDir") }
    foreach ($e in @($m.files)) {
        $p = Join-Path $backupDir $e.path
        if (-not (Test-Path -LiteralPath $p)) { $problems += "missing: $($e.path)"; continue }
        if ((Get-FileSha256 $p) -ne $e.sha256) { $problems += "hash mismatch: $($e.path)" }
    }
    return $problems
}

# Takes a verified backup of the save root. With -IfMissing, returns the newest
# existing complete backup instead of taking a new one. Returns the backup path.
function Backup-Saves([string]$saveRoot = (Get-SaveRoot), [string]$backupRoot = (Get-BackupRoot), [switch]$ifMissing,
                      [string]$prefix = 'saves') {
    if ($ifMissing) {
        $existing = @(Get-SaveBackups $backupRoot)
        if ($existing.Count -gt 0) {
            Write-Step "Save backup exists: $($existing[0])"
            return $existing[0]
        }
    }
    if (@(Get-GameProcesses).Count -gt 0) { throw 'The game is running; refusing to back up saves while it may write them.' }
    $files = @(Get-ScopeFiles $saveRoot)
    if ($files.Count -eq 0) { throw "No save files found under $saveRoot (looked in: $($script:BackupScopes -join ', '))" }
    $dest = Join-Path $backupRoot ($prefix + '-' + (Get-Date).ToString('yyyyMMdd-HHmmss'))
    if (Test-Path -LiteralPath $dest) { throw "$dest already exists" }
    Ensure-Dir $dest
    Write-Step "Backing up $($files.Count) files from $saveRoot to $dest"
    $entries = @()
    $total = 0
    foreach ($f in $files) {
        $rel = Get-RelativePath $saveRoot $f.FullName
        $target = Join-Path $dest $rel
        Ensure-Dir (Split-Path $target -Parent)
        Copy-Item -LiteralPath $f.FullName -Destination $target -Force
        $hs = Get-FileSha256 $f.FullName
        $hd = Get-FileSha256 $target
        if ($hs -ne $hd) { throw "Verification failed for $rel (source $hs, copy $hd); backup left incomplete in $dest" }
        $entries += [pscustomobject]@{ path = $rel; size = $f.Length; sha256 = $hs; lastWriteUtc = $f.LastWriteTimeUtc.ToString('o') }
        $total += $f.Length
    }
    $manifest = [pscustomobject]@{
        created  = (Get-Date).ToString('o')
        source   = $saveRoot
        scopes   = $script:BackupScopes
        count    = $entries.Count
        bytes    = $total
        files    = $entries
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $dest 'backup-manifest.json') -Encoding UTF8
    Write-Step ("Save backup verified: {0} files, {1:N1} MB, SHA-256 checked" -f $entries.Count, ($total / 1MB))
    return $dest
}

# ------------------------------------------------------------------ deploy
# Deploying copies our files into the game's End\Binaries\Win64 and records
# them (name, size, SHA-256) in ff7vr.deploy-manifest.json in that folder.
# Undeploying removes exactly the recorded files, plus the files the mod
# itself creates at runtime (ff7vr.log, ff7vr-crash-*.dmp), which are first
# archived to captures\runs\<time>\ in the repo.

$script:DeployFiles     = @('xinput1_3.dll', 'ff7vr.ini')
$script:RuntimePatterns = @('ff7vr.log', 'ff7vr-crash-*.dmp')

function Get-ManifestPath { return (Join-Path (Get-GameBinDir) $script:ManifestName) }

function Read-DeployManifest {
    $p = Get-ManifestPath
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    return (Get-Content -LiteralPath $p -Raw | ConvertFrom-Json)
}

function Get-BuildOutputDir([string]$buildDir) { return (Join-Path (Resolve-BuildDir $buildDir) 'src\loader') }

function Invoke-Deploy([string]$buildDir, [string]$iniPath = '') {
    $src = Get-BuildOutputDir $buildDir
    $bin = Get-GameBinDir
    if (@(Get-GameProcesses).Count -gt 0) { throw 'The game is running; stop it before deploying (tools\dev\stop.ps1).' }
    $sources = @{}
    foreach ($name in $script:DeployFiles) { $sources[$name] = Join-Path $src $name }
    if ($iniPath) { $sources['ff7vr.ini'] = (Resolve-Path -LiteralPath $iniPath).Path }
    foreach ($name in $script:DeployFiles) {
        if (-not (Test-Path -LiteralPath $sources[$name])) { throw "Missing build output $($sources[$name]); run tools\dev\build.ps1 first" }
    }
    $old = Read-DeployManifest
    $ours = @{}
    if ($old) { foreach ($e in @($old.files)) { $ours[$e.name] = $e.sha256 } }
    # Never overwrite a file we did not put there.
    foreach ($name in $script:DeployFiles) {
        $dst = Join-Path $bin $name
        if ((Test-Path -LiteralPath $dst) -and -not $ours.ContainsKey($name)) {
            throw "$dst exists and was not deployed by these scripts; refusing to overwrite it"
        }
    }
    $entries = @()
    foreach ($name in $script:DeployFiles) {
        $dst = Join-Path $bin $name
        Copy-Item -LiteralPath $sources[$name] -Destination $dst -Force
        $h = Get-FileSha256 $dst
        if ($h -ne (Get-FileSha256 $sources[$name])) { throw "Copy verification failed for $name" }
        $entries += [pscustomobject]@{ name = $name; size = (Get-Item -LiteralPath $dst).Length; sha256 = $h; source = $sources[$name] }
    }
    $manifest = [pscustomobject]@{ deployed = (Get-Date).ToString('o'); repo = $script:RepoRoot; files = $entries;
                                   runtime = $script:RuntimePatterns }
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Get-ManifestPath) -Encoding UTF8
    Write-Step "Deployed $($entries.Count) files to $bin"
    return $entries
}

# Writes a copy of $baseIni with "section.key=value" overrides appended as
# their own [section] blocks. The mod's ini reader keeps the last value of a
# key, so the overrides win. The base file may be missing (overrides only).
function New-OverrideIni([string]$baseIni, [string[]]$overrides, [string]$outPath) {
    $lines = @()
    if ($baseIni -and (Test-Path -LiteralPath $baseIni)) { $lines += @(Get-Content -LiteralPath $baseIni) }
    $lines += ''
    $lines += '; ---- overrides for this run ----'
    foreach ($o in $overrides) {
        $o = $o.Trim()
        if ($o -notmatch '^([^.=\s]+)\.([^=]+?)\s*=(.*)$') { throw "Bad ini override '$o' (expected section.key=value)" }
        $lines += "[$($Matches[1])]"
        $lines += ("{0} = {1}" -f $Matches[2].Trim(), $Matches[3].Trim())
    }
    Ensure-Dir (Split-Path $outPath -Parent)
    [System.IO.File]::WriteAllLines($outPath, [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
}

# Archives runtime files (log, dumps) from the game folder to captures\runs\<stamp>\ and
# deletes them from the game folder. Returns the archive folder or $null if there was nothing.
function Save-RuntimeFiles([string]$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')) {
    $bin = Get-GameBinDir
    $found = @()
    foreach ($pat in $script:RuntimePatterns) { $found += @(Get-ChildItem -LiteralPath $bin -Filter $pat -File -ErrorAction SilentlyContinue) }
    if ($found.Count -eq 0) { return $null }
    $dest = Join-Path $script:CapturesDir ('runs\' + $stamp)
    Ensure-Dir $dest
    foreach ($f in $found) {
        Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $dest $f.Name) -Force
        Remove-Item -LiteralPath $f.FullName -Force
    }
    Write-Step "Archived $($found.Count) runtime file(s) to $dest"
    return $dest
}

# Removes what Invoke-Deploy added. Returns $true if the game folder is clean afterwards.
function Invoke-Undeploy([switch]$force) {
    $bin = Get-GameBinDir
    if (@(Get-GameProcesses).Count -gt 0) { throw 'The game is running; stop it before undeploying (tools\dev\stop.ps1).' }
    [void](Save-RuntimeFiles)
    $m = Read-DeployManifest
    if (-not $m) {
        Write-Step 'Nothing deployed (no manifest)'
        return $true
    }
    $clean = $true
    foreach ($e in @($m.files)) {
        $p = Join-Path $bin $e.name
        if (-not (Test-Path -LiteralPath $p)) { continue }
        if ((Get-FileSha256 $p) -ne $e.sha256 -and -not $force) {
            Write-Step "WARNING: $p changed since it was deployed; left in place (use -Force to remove it)"
            $clean = $false
            continue
        }
        $tries = 0
        while ($true) {
            try { Remove-Item -LiteralPath $p -Force -ErrorAction Stop; break }
            catch {
                $tries++
                if ($tries -ge 20) { throw "Could not remove ${p}: $_" }
                Start-Sleep -Milliseconds 500
            }
        }
    }
    if ($clean) {
        Remove-Item -LiteralPath (Get-ManifestPath) -Force
        Write-Step "Undeployed from $bin"
    }
    return $clean
}

# ------------------------------------------------------------------ window capture
if (-not ('FF7VR.Capture' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
namespace FF7VR {
public static class Capture {
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] struct POINT { public int X, Y; }
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);

    // Per-monitor DPI awareness for this thread, so window sizes are in physical pixels.
    public static void DpiAware() { try { SetThreadDpiAwarenessContext(new IntPtr(-4)); } catch { } }

    // PrintWindow with PW_RENDERFULLCONTENT (2): asks DWM for the composed
    // window content, which also works for D3D flip-model swap chains and for
    // windows that are covered by other windows. Returns the client area.
    public static Bitmap PrintClient(IntPtr hwnd) {
        DpiAware();
        RECT wr, cr; GetWindowRect(hwnd, out wr); GetClientRect(hwnd, out cr);
        int ww = wr.Right - wr.Left, wh = wr.Bottom - wr.Top;
        if (ww <= 0 || wh <= 0 || cr.Right <= 0 || cr.Bottom <= 0) return null;
        POINT origin = new POINT(); ClientToScreen(hwnd, ref origin);
        var full = new Bitmap(ww, wh, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(full)) {
            IntPtr hdc = g.GetHdc();
            bool ok = PrintWindow(hwnd, hdc, 2);
            g.ReleaseHdc(hdc);
            if (!ok) { full.Dispose(); return null; }
        }
        var crop = new Rectangle(origin.X - wr.Left, origin.Y - wr.Top, cr.Right, cr.Bottom);
        crop.Intersect(new Rectangle(0, 0, ww, wh));
        var client = full.Clone(crop, PixelFormat.Format24bppRgb);
        full.Dispose();
        return client;
    }

    // Plain screen copy of the client area. Only correct when the window is visible and on top.
    public static Bitmap ScreenClient(IntPtr hwnd) {
        DpiAware();
        RECT cr; GetClientRect(hwnd, out cr);
        if (cr.Right <= 0 || cr.Bottom <= 0) return null;
        POINT origin = new POINT(); ClientToScreen(hwnd, ref origin);
        var bmp = new Bitmap(cr.Right, cr.Bottom, PixelFormat.Format24bppRgb);
        using (var g = Graphics.FromImage(bmp)) g.CopyFromScreen(origin.X, origin.Y, 0, 0, new Size(cr.Right, cr.Bottom));
        return bmp;
    }

    // {mean luma 0..255, luma standard deviation, fraction of pixels brighter than 16}
    public static double[] Stats(Bitmap b) {
        var data = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        try {
            int stride = data.Stride; byte[] row = new byte[stride];
            double sum = 0, sq = 0; long n = 0, lit = 0;
            for (int y = 0; y < b.Height; y += 4) {
                Marshal.Copy(IntPtr.Add(data.Scan0, y * stride), row, 0, stride);
                for (int x = 0; x < b.Width; x += 4) {
                    int i = x * 3;
                    double l = 0.114 * row[i] + 0.587 * row[i + 1] + 0.299 * row[i + 2];
                    sum += l; sq += l * l; n++; if (l > 16) lit++;
                }
            }
            if (n == 0) return new double[] { 0, 0, 0 };
            double mean = sum / n;
            return new double[] { mean, Math.Sqrt(Math.Max(0, sq / n - mean * mean)), (double)lit / n };
        } finally { b.UnlockBits(data); }
    }
}
}
'@
}

# Captures the game window to a PNG. Method 'print' (default) uses PrintWindow
# with full-content rendering and works while the window is covered; 'screen'
# copies the screen area and needs the window on top. Returns an object with
# Path, Width, Height, Mean, StdDev, Lit and Blank (true for a black/flat frame).
function Save-GameScreenshot([string]$path, [string]$method = 'print', [IntPtr]$hwnd = [IntPtr]::Zero) {
    if ($hwnd -eq [IntPtr]::Zero) { $hwnd = Get-GameWindow }
    if ($hwnd -eq [IntPtr]::Zero) { throw 'Game window not found' }
    $bmp = $null
    if ($method -eq 'screen') {
        [void](Set-GameForeground $hwnd)
        Start-Sleep -Milliseconds 300
        $bmp = [FF7VR.Capture]::ScreenClient($hwnd)
    } else {
        $bmp = [FF7VR.Capture]::PrintClient($hwnd)
    }
    if (-not $bmp) { throw "Capture failed (method $method)" }
    try {
        Ensure-Dir (Split-Path $path -Parent)
        $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        $s = [FF7VR.Capture]::Stats($bmp)
        return [pscustomobject]@{ Path = $path; Width = $bmp.Width; Height = $bmp.Height; Method = $method;
                                  Mean = [math]::Round($s[0], 1); StdDev = [math]::Round($s[1], 1);
                                  Lit = [math]::Round($s[2], 3); Blank = ($s[1] -lt 2.0 -and $s[0] -lt 8) }
    } finally { $bmp.Dispose() }
}

function New-CapturePath([string]$tag = 'shot') {
    return (Join-Path $script:CapturesDir ((Get-Date).ToString('yyyyMMdd-HHmmss') + "-$tag.png"))
}

# ------------------------------------------------------------------ dev pipe (virtual pad, log markers)
# Sends command lines to \\.\pipe\ff7vr-dev (see src\loader\dev_input.h) and returns the replies.
function Send-DevCommand([string[]]$lines, [int]$connectTimeoutMs = 3000) {
    $client = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'ff7vr-dev', [System.IO.Pipes.PipeDirection]::InOut)
    try {
        $client.Connect($connectTimeoutMs)
        $writer = New-Object System.IO.StreamWriter($client)
        $writer.AutoFlush = $true
        $reader = New-Object System.IO.StreamReader($client)
        $replies = @()
        foreach ($l in $lines) {
            if (-not $l -or -not $l.Trim()) { continue }
            $writer.Write($l.Trim() + "`n")
            $replies += $reader.ReadLine()
        }
        return $replies
    } finally { $client.Dispose() }
}

function Test-DevPipe {
    try { $r = @(Send-DevCommand @('ping') 1000); return ($r.Count -gt 0 -and $r[0] -like 'ok*') } catch { return $false }
}

# ------------------------------------------------------------------ screen state
# Rough classification of what the game shows, from colour statistics of a few
# fixed regions of the client area (given as fractions, so any resolution
# works). Calibrated on the English UI at 16:9. States:
#   none        no game window
#   black       (nearly) black frame: start-up, fades, movies between scenes
#   title       "PRESS ANY BUTTON TO CONTINUE" title screen
#   menu        title menu with the cursor on Continue
#   menu-other  title menu with the cursor elsewhere (e.g. New Game)
#   resume      "Resume playing from where you left off?" with Yes selected
#   dialog      another centred dialog (e.g. "Please launch the game via the Steam client")
#   loading     loading screen (blue glow top left)
#   other       anything else; after a loading screen this is gameplay or a cutscene
if (-not ('FF7VR.Regions' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
namespace FF7VR {
public static class Regions {
    // {mean luma, fraction of UI-blue pixels, fraction of pixels with luma > 40}
    public static double[] Stats(Bitmap b, double fx0, double fy0, double fx1, double fy1) {
        int x0 = (int)(fx0 * b.Width), y0 = (int)(fy0 * b.Height), x1 = (int)(fx1 * b.Width), y1 = (int)(fy1 * b.Height);
        var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        try {
            byte[] row = new byte[d.Stride]; double s = 0; long n = 0, blue = 0, lit = 0;
            for (int y = y0; y < y1; y += 2) {
                Marshal.Copy(IntPtr.Add(d.Scan0, y * d.Stride), row, 0, d.Stride);
                for (int x = x0; x < x1; x += 2) {
                    int i = x * 3; int B = row[i], G = row[i + 1], R = row[i + 2];
                    double l = 0.114 * B + 0.587 * G + 0.299 * R;
                    s += l; n++;
                    if (B > 120 && B > R + 50 && B > G + 10) blue++;
                    if (l > 40) lit++;
                }
            }
            if (n == 0) return new double[] { 0, 0, 0 };
            return new double[] { s / n, (double)blue / n, (double)lit / n };
        } finally { b.UnlockBits(d); }
    }
}
}
'@
}

$script:StateRegions = @{
    full      = @(0, 0, 1, 1)
    titleText = @(0.39, 0.74, 0.61, 0.775)
    menuCol   = @(0.085, 0.535, 0.32, 0.88)
    menuCont  = @(0.085, 0.595, 0.32, 0.645)
    menuNew   = @(0.085, 0.538, 0.32, 0.586)
    dlgBox    = @(0.34, 0.3, 0.66, 0.7)
    dlgTop    = @(0.4, 0.295, 0.6, 0.335)
    dlgYes    = @(0.4, 0.575, 0.6, 0.61)
    loadGlow  = @(0, 0, 0.15, 0.2)
}

function Get-FrameState([System.Drawing.Bitmap]$bmp) {
    $f = @{}
    foreach ($k in $script:StateRegions.Keys) {
        $r = $script:StateRegions[$k]
        $f[$k] = [FF7VR.Regions]::Stats($bmp, $r[0], $r[1], $r[2], $r[3])
    }
    # index 0 = mean luma, 1 = blue fraction, 2 = lit fraction
    $state = 'other'
    if ($f.full[0] -lt 3) { $state = 'black' }
    elseif ($f.loadGlow[1] -gt 0.5) { $state = 'loading' }
    elseif ($f.dlgBox[1] -gt 0.12 -and $f.dlgYes[1] -gt 0.3 -and $f.dlgTop[1] -gt 0.2) { $state = 'resume' }
    elseif ($f.dlgBox[1] -gt 0.12) { $state = 'dialog' }
    elseif ($f.menuCont[1] -gt 0.15 -and $f.menuCol[2] -gt 0.05) { $state = 'menu' }
    elseif ($f.menuNew[1] -gt 0.15 -or ($f.menuCol[1] -gt 0.03 -and $f.menuCol[2] -gt 0.05 -and $f.full[0] -lt 40)) { $state = 'menu-other' }
    elseif ($f.titleText[2] -gt 0.25 -and $f.full[0] -lt 12 -and $f.menuCol[2] -lt 0.02) { $state = 'title' }
    return [pscustomobject]@{ State = $state; Mean = [math]::Round($f.full[0], 1); Features = $f }
}

# Captures the game window (PrintWindow) and classifies it. Optionally saves the PNG.
function Get-ScreenState([string]$savePath = '') {
    $hwnd = Get-GameWindow
    if ($hwnd -eq [IntPtr]::Zero) { return [pscustomobject]@{ State = 'none'; Mean = 0; Features = $null } }
    $bmp = [FF7VR.Capture]::PrintClient($hwnd)
    if (-not $bmp) { return [pscustomobject]@{ State = 'none'; Mean = 0; Features = $null } }
    try {
        if ($savePath) { Ensure-Dir (Split-Path $savePath -Parent); $bmp.Save($savePath, [System.Drawing.Imaging.ImageFormat]::Png) }
        return (Get-FrameState $bmp)
    } finally { $bmp.Dispose() }
}

# Polls until the screen is in one of $states (returns the state) or the timeout expires ($null).
function Wait-ScreenState([string[]]$states, [int]$timeoutSeconds, [int]$pollMs = 1000) {
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    $last = ''
    while ((Get-Date) -lt $deadline) {
        if (@(Get-GameProcesses).Count -eq 0) { Write-Step 'Game process exited'; return $null }
        $s = (Get-ScreenState).State
        if ($s -ne $last) { Write-Step "  screen: $s"; $last = $s }
        if ($states -contains $s) { return $s }
        Start-Sleep -Milliseconds $pollMs
    }
    Write-Step "Timed out after $timeoutSeconds s waiting for screen state $($states -join '/') (last: $last)"
    return $null
}

# ------------------------------------------------------------------ keyboard input
# Keys go through SendInput as scan codes, so the game window must be in the
# foreground; Send-GameKey brings it there first.
$script:KeyCodes = @{
    'enter' = 0x0D; 'return' = 0x0D; 'esc' = 0x1B; 'escape' = 0x1B; 'space' = 0x20; 'tab' = 0x09; 'backspace' = 0x08
    'up' = 0x26; 'down' = 0x28; 'left' = 0x25; 'right' = 0x27; 'pageup' = 0x21; 'pagedown' = 0x22
    'home' = 0x24; 'end' = 0x23; 'insert' = 0x2D; 'delete' = 0x2E
    'shift' = 0x10; 'ctrl' = 0x11; 'alt' = 0x12
}
$script:ExtendedKeys = @(0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E)

function Get-KeyCode([string]$name) {
    $n = $name.Trim().ToLowerInvariant()
    if ($script:KeyCodes.ContainsKey($n)) { return [int]$script:KeyCodes[$n] }
    if ($n -match '^[a-z0-9]$') { return [int][char]$n.ToUpperInvariant() }
    if ($n -match '^f([1-9]|1[0-2])$') { return 0x6F + [int]$Matches[1] }
    throw "Unknown key '$name'"
}

function Send-GameKey([string]$name, [int]$holdMs = 120) {
    $hwnd = Get-GameWindow
    if ($hwnd -eq [IntPtr]::Zero) { throw 'Game window not found' }
    if (-not (Set-GameForeground $hwnd)) { Write-Step 'WARNING: could not bring the game window to the foreground; key may be lost' }
    $vk = Get-KeyCode $name
    $sent = [FF7VR.Native]::KeyTap([uint16]$vk, $holdMs, ($script:ExtendedKeys -contains $vk))
    if ($sent -ne 2) { Write-Step "WARNING: SendInput delivered $sent of 2 events for '$name'" }
}

# ------------------------------------------------------------------ game start / stop

function Test-SteamRunning {
    $pid0 = 0
    try { $pid0 = [int](Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam\ActiveProcess' -Name pid -ErrorAction Stop).pid } catch { }
    if ($pid0 -gt 0 -and (Get-Process -Id $pid0 -ErrorAction SilentlyContinue)) { return $true }
    return (@(Get-Process -Name steam -ErrorAction SilentlyContinue).Count -gt 0)
}

# Starts Steam minimised if it is not running and waits until its client is up.
function Start-SteamIfNeeded([int]$timeoutSeconds = 90) {
    if (Test-SteamRunning) { return $true }
    $steam = Get-SteamExe
    if (-not $steam -or -not (Test-Path -LiteralPath $steam)) { throw 'Steam is not running and steam.exe was not found' }
    Write-Step 'Steam is not running; starting it (-silent)'
    Start-Process -FilePath $steam -ArgumentList '-silent' | Out-Null
    $ok = Wait-Until { Test-SteamRunning } $timeoutSeconds 1000 'Steam to start'
    if ($ok) { Start-Sleep -Seconds 10 }  # let the client log in before the game asks for it
    return [bool]$ok
}

# Builds the game's command line. -d3d11 always; windowed at WxH unless $fullscreen.
function Get-GameArguments([int]$width = 1280, [int]$height = 720, [switch]$fullscreen, [string]$extra = '') {
    $a = @('-d3d11')
    if (-not $fullscreen) { $a += @('-windowed', "-ResX=$width", "-ResY=$height") }
    if ($extra) { $a += $extra }
    return ($a -join ' ')
}

# Starts ff7remake_.exe directly (not through Steam), so the command line and
# the environment of this script reach the game. SteamAppId/SteamGameId make
# the Steam API accept a launch that did not come from the Steam client;
# without them the game shows "Please launch the game via the Steam client"
# and exits. $envVars: hashtable of extra environment variables for the game.
function Start-GameDirect([string]$arguments, [hashtable]$envVars = @{}) {
    $saved = @{}
    $vars = @{ 'SteamAppId' = "$script:SteamAppId"; 'SteamGameId' = "$script:SteamAppId" }
    foreach ($k in $envVars.Keys) { $vars[$k] = [string]$envVars[$k] }
    foreach ($k in $vars.Keys) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process')
        [Environment]::SetEnvironmentVariable($k, $vars[$k], 'Process')
    }
    try {
        Write-Step "Starting $(Get-GameExe) $arguments"
        return (Start-Process -FilePath (Get-GameExe) -ArgumentList $arguments -WorkingDirectory (Get-GameBinDir) -PassThru)
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
    }
}

# Asks Steam to launch the game (steam.exe -applaunch). Steam adds the launch
# options set in the game's Steam properties; the game does NOT inherit this
# script's environment.
function Start-GameViaSteam([string]$arguments) {
    $steam = Get-SteamExe
    if (-not $steam) { throw 'steam.exe not found' }
    Write-Step "Starting through Steam: -applaunch $script:SteamAppId $arguments"
    Start-Process -FilePath $steam -ArgumentList ("-applaunch $script:SteamAppId " + $arguments) | Out-Null
}

# Kills the game and its helper processes and waits until they are gone.
function Stop-GameProcesses([int]$timeoutSeconds = 30) {
    $procs = @(Get-GameProcesses) + @(Get-GameHelperProcesses)
    foreach ($p in $procs) {
        Write-Step "Stopping $($p.Name) (pid $($p.Id))"
        try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch { }
    }
    $gone = Wait-Until { (@(Get-GameProcesses).Count + @(Get-GameHelperProcesses).Count) -eq 0 } $timeoutSeconds 500 'game processes to exit'
    if (-not $gone) { throw 'Game processes are still running after kill' }
    # The loader releases the DLL file a moment after the process is reported gone.
    Start-Sleep -Milliseconds 500
}

# Full cleanup after a run: kill, undeploy (archives the log), restore Luma, release the lock.
# Refuses (returns $false) if another owner holds the lock, unless $force.
function Stop-GameRun([string]$owner = (Get-DefaultOwner), [switch]$force) {
    $info = Get-LockInfo
    if ($info -and $info.Owner -ne $owner -and -not $force) {
        Write-Step "The game lock is held by '$($info.Owner)' ($($info.Text)); not touching their run. Use -Force to override."
        return $false
    }
    $ok = $true
    try { Stop-GameProcesses } catch { Write-Step "ERROR: $_"; $ok = $false }
    if ($ok) {
        try { if (-not (Invoke-Undeploy)) { $ok = $false } } catch { Write-Step "ERROR: undeploy: $_"; $ok = $false }
    }
    try { Write-Step ("Luma/ReShade dxgi.dll: " + (Restore-Luma)) } catch { Write-Step "ERROR: $_"; $ok = $false }
    if ($info) { [void](Unlock-Game -owner $info.Owner -force) }
    return $ok
}

# ------------------------------------------------------------------ title screen to gameplay
# Drives the title screen into the most recent save: title -> Enter -> menu with
# Continue selected -> Enter -> "Resume playing...?" Yes -> Enter -> loading ->
# gameplay. Only ever loads; it never selects New Game, and it presses Enter only
# when the screen shows the expected state. Returns $true when gameplay is
# reached (the loading screen was seen and has been gone for $settleSeconds).
function Invoke-ReachGameplay([int]$timeoutSeconds = 240, [int]$settleSeconds = 6, [string]$shotDir = '') {
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    $seenLoading = $false
    $otherSince = $null
    $last = ''
    $presses = 0
    while ((Get-Date) -lt $deadline) {
        if (@(Get-GameProcesses).Count -eq 0) { Write-Step 'Game process exited'; return $false }
        $st = Get-ScreenState
        $s = $st.State
        if ($s -ne $last) {
            Write-Step "  screen: $s (mean $($st.Mean))"
            if ($shotDir) { [void](Get-ScreenState (Join-Path $shotDir ("{0:D2}-{1}.png" -f $presses, $s))) }
            $last = $s
        }
        switch ($s) {
            'title'   { Send-GameKey 'enter'; $presses++; Start-Sleep -Milliseconds 1500 }
            'menu'    { Send-GameKey 'enter'; $presses++; Start-Sleep -Milliseconds 1500 }
            'resume'  { Send-GameKey 'enter'; $presses++; Start-Sleep -Milliseconds 1500 }
            'menu-other' {
                Write-Step 'Title menu cursor is not on Continue; pressing Escape to go back to the title screen'
                Send-GameKey 'escape'; Start-Sleep -Milliseconds 1500
            }
            'dialog'  {
                Write-Step 'Unexpected dialog (not the resume prompt); stopping here'
                if ($shotDir) { [void](Get-ScreenState (Join-Path $shotDir 'unexpected-dialog.png')) }
                return $false
            }
            'loading' { $seenLoading = $true; $otherSince = $null; Start-Sleep -Milliseconds 1000 }
            default {
                if ($seenLoading -and $s -eq 'other') {
                    if (-not $otherSince) { $otherSince = Get-Date }
                    if (((Get-Date) - $otherSince).TotalSeconds -ge $settleSeconds) { return $true }
                } else { $otherSince = $null }
                Start-Sleep -Milliseconds 1000
            }
        }
        if ($presses -gt 12) { Write-Step 'Too many key presses without reaching gameplay; giving up'; return $false }
    }
    Write-Step "Timed out after $timeoutSeconds s (last screen: $last)"
    return $false
}
