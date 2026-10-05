# Shared helpers for the benchmark scripts in tools/bench. Dot-source it:
#   . "$PSScriptRoot\bench-common.ps1"
# Windows PowerShell 5.1 compatible. Builds on tools/dev/common.ps1 (game
# start/stop, locks, Luma rename, save backup, deploy) and
# tools/dev/steamvr-common.ps1 (SteamVR paths and processes).
#
# Environment overrides (nothing machine-specific is stored in the repo):
#   FF7VR_UEVR_DIR   folder with UEVRBackend.dll to use. Default: detected,
#                    see Find-UevrBuild.

Set-StrictMode -Version 2.0
. "$PSScriptRoot\..\dev\common.ps1"
. "$PSScriptRoot\..\dev\steamvr-common.ps1"

$script:BenchDir        = $PSScriptRoot
$script:BenchResultsDir = Join-Path $script:CapturesDir 'bench'       # captures/ is gitignored
$script:UevrStatePath   = Join-Path $script:StateDir 'uevr-profile-run.txt'

# ------------------------------------------------------------------ locks
# Takes the game lock and, if $steamVr, the SteamVR lock, never holding one
# while waiting for the other (so two runs that need both cannot deadlock).
# Returns $true when every requested lock is held.
function Lock-BenchResources([string]$owner, [bool]$steamVr, [int]$waitSeconds) {
    $deadline = (Get-Date).AddSeconds($waitSeconds)
    while ($true) {
        if (Lock-Game -owner $owner -waitSeconds 0) {
            if (-not $steamVr) { return $true }
            if (Lock-SteamVr -owner $owner -waitSeconds 0) { return $true }
            [void](Unlock-Game -owner $owner)
        }
        if ((Get-Date) -ge $deadline) { return $false }
        Write-Step 'Waiting 30 s for the game/SteamVR locks'
        Start-Sleep -Seconds 30
    }
}

# ------------------------------------------------------------------ DLL injection
# What UEVR's own injector (UEVRInjector.exe) does when "Inject" is pressed,
# reproduced without its GUI: LoadLibraryW in the game through a remote
# thread for each DLL, and a remote call of an exported function.
if (-not ('FF7VR.Inject' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
namespace FF7VR {
public static class Inject {
    const uint PROCESS_ACCESS = 0x0002 | 0x0008 | 0x0010 | 0x0020 | 0x0400; // CREATE_THREAD | VM_OPERATION | VM_READ | VM_WRITE | QUERY_INFORMATION
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr addr, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool VirtualFreeEx(IntPtr h, IntPtr addr, UIntPtr size, uint type);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool WriteProcessMemory(IntPtr h, IntPtr addr, byte[] buf, UIntPtr size, out UIntPtr written);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr attr, UIntPtr stack, IntPtr start, IntPtr param, uint flags, out uint tid);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetExitCodeThread(IntPtr h, out uint code);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr GetModuleHandleW(string name);
    [DllImport("kernel32.dll", CharSet=CharSet.Ansi, SetLastError=true)] static extern IntPtr GetProcAddress(IntPtr mod, string name);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool FreeLibrary(IntPtr mod);

    // Base address of a module loaded in the target, or zero.
    public static IntPtr RemoteModule(int pid, string fileName) {
        var p = Process.GetProcessById(pid);
        p.Refresh();
        foreach (ProcessModule m in p.Modules) {
            if (string.Equals(Path.GetFileName(m.FileName), fileName, StringComparison.OrdinalIgnoreCase)) return m.BaseAddress;
        }
        return IntPtr.Zero;
    }

    static uint RunRemote(IntPtr proc, IntPtr start, IntPtr param, uint timeoutMs) {
        uint tid;
        IntPtr th = CreateRemoteThread(proc, IntPtr.Zero, UIntPtr.Zero, start, param, 0, out tid);
        if (th == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateRemoteThread");
        try {
            uint w = WaitForSingleObject(th, timeoutMs);
            if (w != 0) throw new TimeoutException("remote thread did not finish within " + timeoutMs + " ms");
            uint code; GetExitCodeThread(th, out code);
            return code;
        } finally { CloseHandle(th); }
    }

    // LoadLibraryW(path) inside the target. Returns the module base.
    public static IntPtr LoadDll(int pid, string path, uint timeoutMs) {
        if (!File.Exists(path)) throw new FileNotFoundException(path);
        IntPtr proc = OpenProcess(PROCESS_ACCESS, false, pid);
        if (proc == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess");
        try {
            byte[] bytes = Encoding.Unicode.GetBytes(path + "\0");
            IntPtr mem = VirtualAllocEx(proc, IntPtr.Zero, (UIntPtr)bytes.Length, 0x3000, 0x04);
            if (mem == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "VirtualAllocEx");
            try {
                UIntPtr written;
                if (!WriteProcessMemory(proc, mem, bytes, (UIntPtr)bytes.Length, out written)) throw new Win32Exception(Marshal.GetLastWin32Error(), "WriteProcessMemory");
                // kernel32 is mapped at the same address in every process of a session.
                IntPtr loadLibrary = GetProcAddress(GetModuleHandleW("kernel32.dll"), "LoadLibraryW");
                uint code = RunRemote(proc, loadLibrary, mem, timeoutMs);
                if (code == 0) throw new Exception("LoadLibraryW returned NULL in the target for " + path);
            } finally { VirtualFreeEx(proc, mem, UIntPtr.Zero, 0x8000); }
        } finally { CloseHandle(proc); }
        IntPtr b = RemoteModule(pid, Path.GetFileName(path));
        if (b == IntPtr.Zero) throw new Exception(Path.GetFileName(path) + " is not in the target's module list after LoadLibraryW");
        return b;
    }

    // Calls an exported function with no arguments in the target: the export's
    // offset is taken from a local, unresolved mapping of the same file.
    public static uint CallExport(int pid, IntPtr remoteBase, string localPath, string export, uint timeoutMs) {
        IntPtr local = LoadLibraryExW(localPath, IntPtr.Zero, 0x00000001); // DONT_RESOLVE_DLL_REFERENCES: no DllMain
        if (local == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "LoadLibraryEx " + localPath);
        long offset;
        try {
            IntPtr fn = GetProcAddress(local, export);
            if (fn == IntPtr.Zero) throw new Exception("export " + export + " not found in " + localPath);
            offset = fn.ToInt64() - local.ToInt64();
        } finally { FreeLibrary(local); }
        IntPtr proc = OpenProcess(PROCESS_ACCESS, false, pid);
        if (proc == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess");
        try { return RunRemote(proc, new IntPtr(remoteBase.ToInt64() + offset), IntPtr.Zero, timeoutMs); }
        finally { CloseHandle(proc); }
    }
}
}
'@
}

# ------------------------------------------------------------------ UEVR build and profile

function Get-UevrProfileDir { return (Join-Path $env:APPDATA 'UnrealVRMod\ff7remake_') }

# Commit hash a UEVRBackend.dll was built from (UEVR embeds it as a 40-character string).
function Get-UevrBackendCommit([string]$dir) {
    $dll = Join-Path $dir 'UEVRBackend.dll'
    if (-not (Test-Path -LiteralPath $dll)) { return $null }
    $rev = Join-Path $dir 'revision.txt'
    $bytes = [System.IO.File]::ReadAllBytes($dll)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    $cands = @([regex]::Matches($text, '(?<![0-9a-f])[0-9a-f]{40}(?![0-9a-f])') | ForEach-Object { $_.Value } | Select-Object -Unique)
    # Skip byte-table artefacts such as 000102030405...
    $cands = @($cands | Where-Object { $_ -notmatch '^(?:[0-9a-f]{2})*?(00010203|1011121314|2021222324)' -and ($_.ToCharArray() | Select-Object -Unique).Count -gt 8 })
    if (Test-Path -LiteralPath $rev) {
        $r = (Get-Content -LiteralPath $rev -Raw).Trim()
        if ($cands -contains $r) { return $r }
    }
    if ($cands.Count -ge 1) { return $cands[0] }
    return $null
}

# Facts from a UEVR log.txt: commit, whether the framework and VR came up, the
# OpenXR system, the eye swapchain size, and what was found in the game.
function Get-UevrLogFacts([string]$logPath) {
    $f = [ordered]@{ path = $logPath; commit = $null; tag = $null; commitsPastTag = $null; frameworkInitialized = $false;
                     d3d = $null; openxrSystem = $null; requestedRuntime = $null; swapchainWidth = 0; swapchainHeight = 0;
                     swapchainsCreated = $false; gengineFound = $false; tickFound = $false; stereoDeviceHooked = $false;
                     pluginLines = @(); errors = @(); vrLines = @() }
    if (-not (Test-Path -LiteralPath $logPath)) { return [pscustomobject]$f }
    $lines = @((Read-SharedText $logPath) -split "`r?`n")
    $errs = @{}
    foreach ($l in $lines) {
        if ($l -match 'Commit hash: ([0-9a-f]{40})') { $f.commit = $Matches[1] }
        elseif ($l -match '\] Tag: (.+)$') { $f.tag = $Matches[1].Trim() }
        elseif ($l -match 'Commits past tag: (\d+)') { $f.commitsPastTag = [int]$Matches[1] }
        elseif ($l -match 'Framework initialized') { $f.frameworkInitialized = $true }
        elseif ($l -match 'Hooked DirectX (\d+)') { $f.d3d = 'D3D' + $Matches[1] }
        elseif ($l -match 'OpenXR system Name: (.+)$') { $f.openxrSystem = $Matches[1].Trim() }
        elseif ($l -match 'Requested runtime: (.+)$') { $f.requestedRuntime = $Matches[1].Trim() }
        elseif ($l -match '\[VR\] Width: (\d+)') { $f.swapchainWidth = [int]$Matches[1] }
        elseif ($l -match '\[VR\] Height: (\d+)') { $f.swapchainHeight = [int]$Matches[1] }
        elseif ($l -match 'Successfully created OpenXR swapchains') { $f.swapchainsCreated = $true }
        elseif ($l -match 'Found GEngine') { $f.gengineFound = $true }
        elseif ($l -match 'UGameEngine::Tick: found') { $f.tickFound = $true }
        if ($l -match '\[Plugin\]') { $f.pluginLines += ($l -replace '^\[[^\]]+\] \[UnrealVR\] ', '') }
        if ($l -match 'StereoRenderingDevice|stereo rendering device|IStereoRendering|Native stereo|NativeStereo|Stereo device') {
            $f.stereoDeviceHooked = $true
            if ($f.vrLines.Count -lt 40) { $f.vrLines += ($l -replace '^\[[^\]]+\] \[UnrealVR\] ', '') }
        }
        if ($l -match '\[error\]') {
            $k = ($l -replace '^\[[^\]]+\] ', '')
            if (-not $errs.ContainsKey($k)) { $errs[$k] = 0 }
            $errs[$k]++
        }
    }
    $f.errors = @($errs.Keys | ForEach-Object { '{0} (x{1})' -f $_, $errs[$_] })
    return [pscustomobject]$f
}

# Finds the UEVR build to use. Order: FF7VR_UEVR_DIR; else the folder whose
# UEVRBackend.dll was built from the commit named in the game profile's last
# log.txt (the build the profile was last used with), searched in the usual
# download locations; else nothing.
function Find-UevrBuild([string]$dir = '') {
    if (-not $dir) { $dir = $env:FF7VR_UEVR_DIR }
    if ($dir) {
        if (-not (Test-Path -LiteralPath (Join-Path $dir 'UEVRBackend.dll'))) { throw "No UEVRBackend.dll in $dir" }
        return [pscustomobject]@{ Dir = (Resolve-Path -LiteralPath $dir).Path; Commit = (Get-UevrBackendCommit $dir); Reason = 'given' }
    }
    $want = (Get-UevrLogFacts (Join-Path (Get-UevrProfileDir) 'log.txt')).commit
    $roots = @((Join-Path $env:USERPROFILE 'Downloads'), (Join-Path $env:USERPROFILE 'Documents'), (Join-Path $env:USERPROFILE 'Desktop'))
    $found = @()
    foreach ($r in $roots) {
        if (-not (Test-Path -LiteralPath $r)) { continue }
        foreach ($d in @(Get-ChildItem -LiteralPath $r -Directory -ErrorAction SilentlyContinue)) {
            foreach ($cand in @($d.FullName) + @(Get-ChildItem -LiteralPath $d.FullName -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })) {
                if (Test-Path -LiteralPath (Join-Path $cand 'UEVRBackend.dll')) {
                    $found += [pscustomobject]@{ Dir = $cand; Commit = (Get-UevrBackendCommit $cand) }
                }
            }
        }
    }
    if ($want) {
        $m = @($found | Where-Object { $_.Commit -eq $want })
        if ($m.Count -gt 0) { return [pscustomobject]@{ Dir = $m[0].Dir; Commit = $want; Reason = "matches the profile's log ($($want.Substring(0, 8)))" } }
    }
    $list = ($found | ForEach-Object { "$($_.Dir) [$($_.Commit)]" }) -join '; '
    throw "No UEVR build matching the profile's commit '$want' found (candidates: $list). Set FF7VR_UEVR_DIR or pass -UevrDir."
}

# Snapshot of a folder: relative path -> SHA-256.
function Get-FolderHashes([string]$root) {
    $h = @{}
    if (-not (Test-Path -LiteralPath $root)) { return $h }
    foreach ($f in @(Get-ChildItem -LiteralPath $root -Recurse -File -Force)) {
        $h[(Get-RelativePath $root $f.FullName)] = Get-FileSha256 $f.FullName
    }
    return $h
}

function Compare-FolderToManifest([string]$root, $manifest) {
    $problems = @()
    $live = Get-FolderHashes $root
    $want = @{}
    foreach ($e in @($manifest.files)) { $want[$e.path] = $e.sha256 }
    foreach ($k in $want.Keys) {
        if (-not $live.ContainsKey($k)) { $problems += "missing: $k" }
        elseif ($live[$k] -ne $want[$k]) { $problems += "changed: $k" }
    }
    foreach ($k in $live.Keys) { if (-not $want.ContainsKey($k)) { $problems += "extra: $k" } }
    return $problems
}

function Get-UevrProfileBackups {
    $root = Get-BackupRoot
    if (-not (Test-Path -LiteralPath $root)) { return @() }
    return @(Get-ChildItem -LiteralPath $root -Directory -Filter 'uevr-profile-*' |
             Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'backup-manifest.json') } |
             Sort-Object Name -Descending | ForEach-Object { $_.FullName })
}

# Copies the whole UEVR profile folder to <BackupRoot>\uevr-profile-<time>\files\
# with a SHA-256 manifest, verified file by file. Returns the backup folder.
function Backup-UevrProfile {
    $src = Get-UevrProfileDir
    if (-not (Test-Path -LiteralPath $src)) { throw "UEVR profile folder not found: $src" }
    $dest = Join-Path (Get-BackupRoot) ('uevr-profile-' + (Get-Date).ToString('yyyyMMdd-HHmmss'))
    $files = Join-Path $dest 'files'
    Ensure-Dir $files
    $entries = @()
    foreach ($f in @(Get-ChildItem -LiteralPath $src -Recurse -File -Force)) {
        $rel = Get-RelativePath $src $f.FullName
        $t = Join-Path $files $rel
        Ensure-Dir (Split-Path $t -Parent)
        Copy-Item -LiteralPath $f.FullName -Destination $t -Force
        $hs = Get-FileSha256 $f.FullName
        if ($hs -ne (Get-FileSha256 $t)) { throw "UEVR profile backup verification failed for $rel" }
        $entries += [pscustomobject]@{ path = $rel; size = $f.Length; sha256 = $hs; lastWriteUtc = $f.LastWriteTimeUtc.ToString('o') }
    }
    $dirs = @(Get-ChildItem -LiteralPath $src -Recurse -Directory -Force | ForEach-Object { Get-RelativePath $src $_.FullName })
    [pscustomobject]@{ created = (Get-Date).ToString('o'); source = $src; count = $entries.Count; dirs = $dirs; files = $entries } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $dest 'backup-manifest.json') -Encoding UTF8
    Write-Step "UEVR profile backed up and verified: $($entries.Count) files -> $dest"
    return $dest
}

# Puts the profile folder back exactly as in the backup (removes files UEVR
# added, restores changed ones with their timestamps) and verifies every hash.
# Returns a list of problems (empty = identical).
function Restore-UevrProfile([string]$backup) {
    $dst = Get-UevrProfileDir
    $m = Get-Content -LiteralPath (Join-Path $backup 'backup-manifest.json') -Raw | ConvertFrom-Json
    $files = Join-Path $backup 'files'
    $want = @{}
    foreach ($e in @($m.files)) { $want[$e.path] = $e }
    if (Test-Path -LiteralPath $dst) {
        foreach ($f in @(Get-ChildItem -LiteralPath $dst -Recurse -File -Force)) {
            $rel = Get-RelativePath $dst $f.FullName
            if (-not $want.ContainsKey($rel)) { Remove-Item -LiteralPath $f.FullName -Force }
        }
        $keepDirs = @($m.dirs)
        foreach ($d in @(Get-ChildItem -LiteralPath $dst -Recurse -Directory -Force | Sort-Object { $_.FullName.Length } -Descending)) {
            $rel = Get-RelativePath $dst $d.FullName
            if ($keepDirs -notcontains $rel -and @(Get-ChildItem -LiteralPath $d.FullName -Force).Count -eq 0) { Remove-Item -LiteralPath $d.FullName -Force }
        }
    }
    foreach ($rel in $want.Keys) {
        $t = Join-Path $dst $rel
        if ((Test-Path -LiteralPath $t) -and (Get-FileSha256 $t) -eq $want[$rel].sha256) { continue }
        Ensure-Dir (Split-Path $t -Parent)
        Copy-Item -LiteralPath (Join-Path $files $rel) -Destination $t -Force
    }
    foreach ($d in @($m.dirs)) { Ensure-Dir (Join-Path $dst $d) }
    return @(Compare-FolderToManifest $dst $m)
}

# Before a UEVR run: make sure a verified backup of the current profile exists
# and mark the profile as "in use by a benchmark run". If a previous run did
# not finish its restore, restore first.
function Enter-UevrProfileRun {
    if (Test-Path -LiteralPath $script:UevrStatePath) {
        $prev = (Get-Content -LiteralPath $script:UevrStatePath -Raw).Trim()
        Write-Step "A previous UEVR run did not restore the profile; restoring from $prev"
        $p = @(Restore-UevrProfile $prev)
        if ($p.Count -gt 0) { throw "UEVR profile restore failed: $($p -join ', ')" }
        Remove-Item -LiteralPath $script:UevrStatePath -Force
    }
    $backups = @(Get-UevrProfileBackups)
    $backup = $null
    if ($backups.Count -gt 0) {
        $m = Get-Content -LiteralPath (Join-Path $backups[0] 'backup-manifest.json') -Raw | ConvertFrom-Json
        if (@(Compare-FolderToManifest (Get-UevrProfileDir) $m).Count -eq 0) { $backup = $backups[0]; Write-Step "UEVR profile matches backup $backup" }
        else { Write-Step 'UEVR profile differs from the newest backup (changed since); taking a new backup' }
    }
    if (-not $backup) { $backup = Backup-UevrProfile }
    Ensure-Dir $script:StateDir
    Set-Content -LiteralPath $script:UevrStatePath -Value $backup -Encoding ASCII
    return $backup
}

# After a UEVR run (also after a failed one): restore and verify.
function Exit-UevrProfileRun {
    if (-not (Test-Path -LiteralPath $script:UevrStatePath)) { return @() }
    $backup = (Get-Content -LiteralPath $script:UevrStatePath -Raw).Trim()
    $p = @(Restore-UevrProfile $backup)
    if ($p.Count -eq 0) {
        Remove-Item -LiteralPath $script:UevrStatePath -Force
        Write-Step "UEVR profile restored and verified identical to $backup"
    } else { Write-Step "ERROR: UEVR profile differs from $backup after restore: $($p -join ', ')" }
    return $p
}

# Writes config.txt for one run: the profile's own config with some keys replaced.
function Set-UevrConfigValues([hashtable]$values) {
    $cfg = Join-Path (Get-UevrProfileDir) 'config.txt'
    $lines = @(Get-Content -LiteralPath $cfg)
    $seen = @{}
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^([^=]+)=(.*)$' -and $values.ContainsKey($Matches[1])) {
            $lines[$i] = $Matches[1] + '=' + $values[$Matches[1]]
            $seen[$Matches[1]] = $true
        }
    }
    foreach ($k in $values.Keys) { if (-not $seen.ContainsKey($k)) { $lines += "$k=$($values[$k])" } }
    [System.IO.File]::WriteAllLines($cfg, [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
}

function Get-UevrConfigValues {
    $h = [ordered]@{}
    $cfg = Join-Path (Get-UevrProfileDir) 'config.txt'
    foreach ($l in @(Get-Content -LiteralPath $cfg)) { if ($l -match '^([^=]+)=(.*)$') { $h[$Matches[1]] = $Matches[2] } }
    return $h
}

# Injects UEVR into the running game the way UEVRInjector.exe does with
# OpenXR selected and "Nullify VR plugins" ticked:
#   1. UEVRPluginNullifier.dll, then its export nullify() (renames the
#      engine's own openxr_loader.dll / openvr_api.dll strings so the game's
#      VR plugins cannot load and grab the runtime)
#   2. openxr_loader.dll from the UEVR folder (the runtime UEVR will use)
#   3. UEVRBackend.dll (starts UEVR; it reads the profile and
#      Frontend_RequestedRuntime from config.txt)
function Invoke-UevrInject([int]$gamePid, [string]$uevrDir, [switch]$noNullify) {
    if (-not $noNullify) {
        $n = Join-Path $uevrDir 'UEVRPluginNullifier.dll'
        $b = [FF7VR.Inject]::LoadDll($gamePid, $n, 15000)
        $code = [FF7VR.Inject]::CallExport($gamePid, $b, $n, 'nullify', 15000)
        Write-Step ("UEVR: nullifier loaded at 0x{0:x}, nullify() returned {1}" -f $b.ToInt64(), $code)
    }
    $x = [FF7VR.Inject]::LoadDll($gamePid, (Join-Path $uevrDir 'openxr_loader.dll'), 15000)
    Write-Step ("UEVR: openxr_loader.dll loaded at 0x{0:x}" -f $x.ToInt64())
    $u = [FF7VR.Inject]::LoadDll($gamePid, (Join-Path $uevrDir 'UEVRBackend.dll'), 30000)
    Write-Step ("UEVR: UEVRBackend.dll loaded at 0x{0:x}" -f $u.ToInt64())
}

# Save games (Steam\<account>\*) compared with the newest verified save
# backup. The UE4 crash reporter's per-start ini under Saved\Config is
# ignored (the engine writes one on every start). Returns a list of
# differences (empty = identical).
function Compare-SavesToBackup {
    $b = @(Get-SaveBackups)
    if ($b.Count -eq 0) { return @('no save backup found') }
    $m = Read-BackupManifest $b[0]
    $root = Get-SaveRoot
    $out = @()
    $want = @{}
    foreach ($e in @($m.files)) { if ($e.path -like 'Steam\*') { $want[$e.path] = $e.sha256 } }
    foreach ($k in $want.Keys) {
        $p = Join-Path $root $k
        if (-not (Test-Path -LiteralPath $p)) { $out += "missing: $k" }
        elseif ((Get-FileSha256 $p) -ne $want[$k]) { $out += "changed: $k" }
    }
    $steam = Join-Path $root 'Steam'
    if (Test-Path -LiteralPath $steam) {
        foreach ($f in @(Get-ChildItem -LiteralPath $steam -Recurse -File -Force)) {
            $rel = Get-RelativePath $root $f.FullName
            if (-not $want.ContainsKey($rel)) { $out += "new: $rel" }
        }
    }
    return $out
}

# ------------------------------------------------------------------ OpenXR runtime for a run

function Get-SteamVrRuntimeJson {
    $r = Get-SteamVrRoot
    if (-not $r) { throw 'SteamVR is not installed' }
    return (Join-Path $r 'steamxr_win64.json')
}

# Environment variables that switch off every implicit OpenXR API layer
# registered on this machine (for this process only), so overlays such as
# ReShade's or OpenXR Toolkit's layer do not add their own cost to a VR run.
function Get-ImplicitLayerDisableEnv {
    $out = @{}
    foreach ($k in @('HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit', 'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit')) {
        $item = Get-Item -LiteralPath $k -ErrorAction SilentlyContinue
        if (-not $item) { continue }
        foreach ($manifest in $item.Property) {
            if (-not (Test-Path -LiteralPath $manifest)) { continue }
            try {
                $j = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
                $v = $j.api_layer.disable_environment
                if ($v) { $out[$v] = '1' }
            } catch { }
        }
    }
    return $out
}

# SteamVR's own per-application statistics, written to vrcompositor.txt when
# the application disconnects ("Cumulative stats for pid: N"): presents,
# dropped and reprojected frames, and the application's average CPU and GPU
# frame time as the compositor measured it. Returns $null if not found.
function Get-SteamVrAppStats([int]$gamePid) {
    $steam = Get-SteamRoot
    if (-not $steam) { return $null }
    $log = Join-Path $steam 'logs\vrcompositor.txt'
    if (-not (Test-Path -LiteralPath $log)) { return $null }
    $lines = @((Read-SharedText $log) -split "`r?`n")
    $start = -1
    for ($i = $lines.Count - 1; $i -ge 0; $i--) { if ($lines[$i] -match "Cumulative stats for pid: $gamePid\b") { $start = $i; break } }
    if ($start -lt 0) { return $null }
    $o = [ordered]@{ raw = @() }
    for ($i = $start + 1; $i -lt [math]::Min($lines.Count, $start + 12); $i++) {
        $l = $lines[$i]
        if ($l -match '#####') { break }
        $c = ($l -replace '^.*?\[Info\] - ', '')
        $o.raw += $c
        if ($c -match '^Total\.+\s+(\d+) presents\.\s+(\d+) dropped\.\s+(\d+) reprojected') { $o.presents = [int]$Matches[1]; $o.dropped = [int]$Matches[2]; $o.reprojected = [int]$Matches[3] }
        if ($l -match 'Compositor Time\.+CPU: ([\d.]+)ms / GPU: ([\d.]+)ms') { $o.compositorCpuMs = [double]$Matches[1]; $o.compositorGpuMs = [double]$Matches[2] }
        if ($l -match 'ApplicationTime CPU: ([\d.]+)ms / GPU: ([\d.]+)ms') { $o.appCpuMs = [double]$Matches[1]; $o.appGpuMs = [double]$Matches[2] }
        if ($l -match 'FPS Average Target (\d+)') { $o.targetFps = [int]$Matches[1] }
    }
    return [pscustomobject]$o
}

# ------------------------------------------------------------------ GPU and CPU sampling

function Get-NvidiaSmi {
    $c = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $p = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'
    if (Test-Path -LiteralPath $p) { return $p }
    return $null
}

function Get-GpuInfo {
    $smi = Get-NvidiaSmi
    if (-not $smi) { return $null }
    $l = & $smi --query-gpu=name,driver_version,memory.total,pcie.link.gen.current,power.limit --format=csv,noheader,nounits 2>$null | Select-Object -First 1
    if (-not $l) { return $null }
    $a = $l -split ',\s*'
    return [pscustomobject]@{ name = $a[0]; driver = $a[1]; memoryTotalMiB = [double]$a[2]; powerLimitW = $a[4] }
}

# GPU sampling through NVML (nvml.dll, installed with the NVIDIA driver), on
# a background thread of this script's process: no process start per sample
# and nothing that has to be killed (nvidia-smi -f only writes its file when
# it exits normally).
if (-not ('FF7VR.GpuSampler' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
namespace FF7VR {
public class GpuSample { public double T; public uint Util; public uint MemUtil; public double MemUsedMiB; public double PowerW; public uint ClockMHz; public uint TempC; }
public class GpuSampler {
    [StructLayout(LayoutKind.Sequential)] struct Utilization { public uint gpu; public uint memory; }
    [StructLayout(LayoutKind.Sequential)] struct Memory { public ulong total; public ulong free; public ulong used; }
    [DllImport("nvml.dll")] static extern int nvmlInit_v2();
    [DllImport("nvml.dll")] static extern int nvmlShutdown();
    [DllImport("nvml.dll")] static extern int nvmlDeviceGetHandleByIndex_v2(uint index, out IntPtr device);
    [DllImport("nvml.dll")] static extern int nvmlDeviceGetUtilizationRates(IntPtr device, out Utilization u);
    [DllImport("nvml.dll")] static extern int nvmlDeviceGetMemoryInfo(IntPtr device, out Memory m);
    [DllImport("nvml.dll")] static extern int nvmlDeviceGetPowerUsage(IntPtr device, out uint milliwatts);
    [DllImport("nvml.dll")] static extern int nvmlDeviceGetClockInfo(IntPtr device, int type, out uint mhz);
    [DllImport("nvml.dll")] static extern int nvmlDeviceGetTemperature(IntPtr device, int sensor, out uint c);

    readonly List<GpuSample> samples = new List<GpuSample>();
    Thread thread; volatile bool stop; IntPtr dev; int interval;
    public string Error;

    public bool Start(int intervalMs) {
        interval = intervalMs;
        try {
            int r = nvmlInit_v2();
            if (r != 0) { Error = "nvmlInit failed: " + r; return false; }
            r = nvmlDeviceGetHandleByIndex_v2(0, out dev);
            if (r != 0) { Error = "nvmlDeviceGetHandleByIndex failed: " + r; nvmlShutdown(); return false; }
        } catch (Exception e) { Error = e.Message; return false; }
        thread = new Thread(Run); thread.IsBackground = true; thread.Start();
        return true;
    }
    void Run() {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (!stop) {
            var s = new GpuSample(); s.T = sw.Elapsed.TotalSeconds;
            Utilization u; if (nvmlDeviceGetUtilizationRates(dev, out u) == 0) { s.Util = u.gpu; s.MemUtil = u.memory; }
            Memory m; if (nvmlDeviceGetMemoryInfo(dev, out m) == 0) s.MemUsedMiB = m.used / 1048576.0;
            uint p; if (nvmlDeviceGetPowerUsage(dev, out p) == 0) s.PowerW = p / 1000.0;
            uint c; if (nvmlDeviceGetClockInfo(dev, 0, out c) == 0) s.ClockMHz = c;
            uint t; if (nvmlDeviceGetTemperature(dev, 0, out t) == 0) s.TempC = t;
            lock (samples) samples.Add(s);
            Thread.Sleep(interval);
        }
    }
    public GpuSample[] Stop() {
        stop = true;
        if (thread != null) thread.Join(5000);
        try { nvmlShutdown(); } catch { }
        lock (samples) return samples.ToArray();
    }
}
}
'@
}

# Starts GPU sampling every $intervalMs. Returns the sampler or $null.
function Start-GpuSampler([int]$intervalMs = 250) {
    $s = New-Object FF7VR.GpuSampler
    if (-not $s.Start($intervalMs)) { Write-Step "GPU sampling unavailable: $($s.Error)"; return $null }
    return $s
}

# Stops sampling, writes the samples to $csvPath, returns them.
function Stop-GpuSampler($sampler, [string]$csvPath) {
    if (-not $sampler) { return @() }
    $rows = @($sampler.Stop())
    $lines = @('t_s,util_pct,mem_util_pct,mem_used_mib,power_w,clock_mhz,temp_c')
    foreach ($r in $rows) {
        $lines += [string]::Format([System.Globalization.CultureInfo]::InvariantCulture, '{0:F3},{1},{2},{3:F0},{4:F1},{5},{6}',
                                   $r.T, $r.Util, $r.MemUtil, $r.MemUsedMiB, $r.PowerW, $r.ClockMHz, $r.TempC)
    }
    [System.IO.File]::WriteAllLines($csvPath, [string[]]$lines)
    return $rows
}

# Per-thread CPU time of a process, with thread names where the program set
# one (SetThreadDescription). Used to see whether one thread (game thread,
# render thread) is saturated, which a process-wide CPU figure hides.
if (-not ('FF7VR.Threads' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
namespace FF7VR {
public class ThreadTime { public int Id; public string Name; public double CpuMs; }
public static class Threads {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenThread(uint access, bool inherit, int id);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr desc);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
    public static ThreadTime[] Snapshot(int pid) {
        var list = new List<ThreadTime>();
        Process p;
        try { p = Process.GetProcessById(pid); p.Refresh(); } catch { return list.ToArray(); }
        foreach (ProcessThread t in p.Threads) {
            var tt = new ThreadTime(); tt.Id = t.Id;
            try { tt.CpuMs = t.TotalProcessorTime.TotalMilliseconds; } catch { continue; }
            IntPtr h = OpenThread(0x1000, false, t.Id); // THREAD_QUERY_LIMITED_INFORMATION
            if (h != IntPtr.Zero) {
                IntPtr d;
                if (GetThreadDescription(h, out d) >= 0 && d != IntPtr.Zero) { tt.Name = Marshal.PtrToStringUni(d); LocalFree(d); }
                CloseHandle(h);
            }
            list.Add(tt);
        }
        return list.ToArray();
    }
}
}
'@
}

# Busiest threads between two snapshots: [{ id, name, busyPct }] (100 = one core all the time).
function Get-BusiestThreads($before, $after, [double]$wallMs, [int]$top = 6) {
    $b = @{}
    foreach ($t in $before) { $b[$t.Id] = $t.CpuMs }
    $rows = @()
    foreach ($t in $after) {
        if (-not $b.ContainsKey($t.Id)) { continue }
        $rows += [pscustomobject]@{ id = $t.Id; name = $t.Name; busyPct = [math]::Round(100.0 * ($t.CpuMs - $b[$t.Id]) / $wallMs, 1) }
    }
    return @($rows | Sort-Object busyPct -Descending | Select-Object -First $top)
}

# Captures the largest visible window of a process (PrintWindow) to a PNG.
function Save-ProcessWindow([string]$processName, [string]$path) {
    $best = [IntPtr]::Zero; $bestArea = 0
    foreach ($p in @(Get-Process -Name $processName -ErrorAction SilentlyContinue)) {
        foreach ($h in [FF7VR.Native]::WindowsOfProcess([uint32]$p.Id, $true)) {
            $r = New-Object FF7VR.Native+RECT
            [void][FF7VR.Native]::GetWindowRect($h, [ref]$r)
            $a = ($r.Right - $r.Left) * ($r.Bottom - $r.Top)
            if ($a -gt $bestArea) { $best = $h; $bestArea = $a }
        }
    }
    if ($best -eq [IntPtr]::Zero) { return $null }
    $shot = Save-GameScreenshot -path $path -hwnd $best
    return [pscustomobject]@{ path = $shot.Path; title = [FF7VR.Native]::Title($best); width = $shot.Width; height = $shot.Height; mean = $shot.Mean; blank = $shot.Blank }
}

# Scripted camera motion for the 'pan' scene: relative mouse moves sent with
# SendInput at a fixed rate from a background thread (the game turns the
# camera with the mouse). Input only reaches the game while its window is in
# the foreground.
if (-not ('FF7VR.MousePan' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;
namespace FF7VR {
public class MousePan {
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public MOUSEINPUT mi; }
    [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    Thread thread; volatile bool stop; int dx, dy, interval; public long Sent;
    public void Start(int stepX, int stepY, int intervalMs) {
        dx = stepX; dy = stepY; interval = intervalMs;
        thread = new Thread(Run); thread.IsBackground = true; thread.Start();
    }
    void Run() {
        var inp = new INPUT[1];
        inp[0].type = 0; inp[0].mi.dwFlags = 0x0001; // MOUSEEVENTF_MOVE (relative)
        while (!stop) {
            inp[0].mi.dx = dx; inp[0].mi.dy = dy;
            Sent += SendInput(1, inp, Marshal.SizeOf(typeof(INPUT)));
            Thread.Sleep(interval);
        }
    }
    public long Stop() { stop = true; if (thread != null) thread.Join(2000); return Sent; }
}
}
'@
}

# ------------------------------------------------------------------ statistics

function Get-Percentile([double[]]$sorted, [double]$p) {
    if ($sorted.Count -eq 0) { return 0 }
    $rank = ($p / 100.0) * ($sorted.Count - 1)
    $lo = [math]::Floor($rank)
    $hi = [math]::Ceiling($rank)
    if ($lo -eq $hi) { return $sorted[[int]$lo] }
    return $sorted[[int]$lo] + ($sorted[[int]$hi] - $sorted[[int]$lo]) * ($rank - $lo)
}

# Frame statistics from the frame timer's CSV. Frame times are the intervals
# between consecutive Present calls (the first row has no interval).
function Get-FrameStats([string]$csvPath) {
    $iv = New-Object System.Collections.Generic.List[double]
    $pr = New-Object System.Collections.Generic.List[double]
    $first = $true
    foreach ($l in [System.IO.File]::ReadLines($csvPath)) {
        if ($first) { $first = $false; continue }
        $a = $l.Split(',')
        $i = [double]::Parse($a[2], [System.Globalization.CultureInfo]::InvariantCulture)
        if ($i -gt 0) {
            $iv.Add($i)
            $pr.Add([double]::Parse($a[3], [System.Globalization.CultureInfo]::InvariantCulture))
        }
    }
    if ($iv.Count -lt 2) { return $null }
    $sorted = $iv.ToArray()
    [Array]::Sort($sorted)
    $sum = 0.0; foreach ($v in $sorted) { $sum += $v }
    $n = $sorted.Count
    $worst = [math]::Max(1, [int][math]::Floor($n / 100))
    $wsum = 0.0; for ($k = $n - $worst; $k -lt $n; $k++) { $wsum += $sorted[$k] }
    $prs = $pr.ToArray(); [Array]::Sort($prs)
    # Hitches: frames longer than 50 ms (a stall of the game, the runtime or the machine).
    $hitches = 0; foreach ($v in $sorted) { if ($v -gt 50.0) { $hitches++ } }
    $mean = $sum / $n
    $var = 0.0; foreach ($v in $sorted) { $var += ($v - $mean) * ($v - $mean) }
    return [pscustomobject]@{
        frames         = $n
        seconds        = [math]::Round($sum / 1000.0, 3)
        avgFps         = [math]::Round(1000.0 * $n / $sum, 2)
        frameTimeMs    = [pscustomobject]@{
            mean = [math]::Round($mean, 3); stdev = [math]::Round([math]::Sqrt($var / $n), 3)
            p50 = [math]::Round((Get-Percentile $sorted 50), 3); p95 = [math]::Round((Get-Percentile $sorted 95), 3)
            p99 = [math]::Round((Get-Percentile $sorted 99), 3); max = [math]::Round($sorted[$n - 1], 3); min = [math]::Round($sorted[0], 3)
        }
        # 1% low: frame rate over the slowest 1% of frames (mean of their frame times).
        hitchesOver50ms  = $hitches
        onePercentLowFps = [math]::Round(1000.0 / ($wsum / $worst), 2)
        p99Fps           = [math]::Round(1000.0 / (Get-Percentile $sorted 99), 2)
        # Time spent inside the original Present (blocking on the GPU queue or vsync).
        presentMs        = [pscustomobject]@{ p50 = [math]::Round((Get-Percentile $prs 50), 3); p95 = [math]::Round((Get-Percentile $prs 95), 3) }
    }
}

function Get-MeanStd([double[]]$v) {
    if ($v.Count -eq 0) { return [pscustomobject]@{ mean = $null; std = $null; min = $null; max = $null; n = 0 } }
    $m = ($v | Measure-Object -Average -Minimum -Maximum)
    $s = 0.0
    foreach ($x in $v) { $s += ($x - $m.Average) * ($x - $m.Average) }
    $sd = 0.0
    if ($v.Count -gt 1) { $sd = [math]::Sqrt($s / ($v.Count - 1)) }
    return [pscustomobject]@{ mean = $m.Average; std = $sd; min = $m.Minimum; max = $m.Maximum; n = $v.Count }
}
