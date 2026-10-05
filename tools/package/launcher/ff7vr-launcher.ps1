<#
.SYNOPSIS
  Starts FINAL FANTASY VII REMAKE INTERGRADE with the ff7vr VR mod for one session,
  and puts the game folder back to normal afterwards.

.DESCRIPTION
  start (default)
    1. Finds the game through Steam.
    2. Refuses if the game is already running.
    3. Puts things back first if an earlier session did not finish (launcher
       window closed, crash, restart).
    4. Checks that Steam runs (starts it if not) and warns if Virtual Desktop's
       Streamer does not seem to run (the game then simply runs flat).
    5. Sets ReShade/Luma's dxgi.dll aside as dxgi.dll.vr-disabled, copies the
       mod (xinput1_3.dll) and ff7vr.ini into End\Binaries\Win64, and records
       every change in End\Binaries\Win64\ff7vr.session.json.
    6. Starts the game (-d3d11) and waits until it exits.
    7. Keeps the session's ff7vr.log in logs\<time>\ next to this script,
       removes the mod's files and puts dxgi.dll back.

  restore
    Puts the game folder back to normal: removes the mod's files and puts
    dxgi.dll back. Harmless when nothing needs doing. Refuses while the game
    runs.

  status
    Shows what is in place, changes nothing.

.PARAMETER KeepInstalled
  Leave the mod (and the Luma rename, unless -KeepLuma) in place after the game
  exits, so the game can be started from Steam with the mod. Undo with restore.

.PARAMETER KeepLuma
  Do not set Luma's dxgi.dll aside (runs the mod together with ReShade/Luma,
  which has not been tested).

.PARAMETER GameDir
  The game's install folder (the one containing End\). Found through Steam when
  not given; the environment variable FF7VR_GAME_DIR also works.

.PARAMETER ExtraArgs
  Extra command-line arguments for the game.

.PARAMETER NoPause
  Do not wait for a key press before closing the window.

.EXAMPLE
  start-vr.cmd
  restore.cmd
  powershell -NoProfile -ExecutionPolicy Bypass -File ff7vr-launcher.ps1 start -KeepInstalled
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet('start', 'restore', 'status')]
    [string]$Action = 'start',
    [switch]$KeepInstalled,
    [switch]$KeepLuma,
    [string]$GameDir = '',
    [string]$ExtraArgs = '',
    [switch]$NoPause
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$AppId          = 1462040
$GameProcess    = 'ff7remake_'
$Here           = $PSScriptRoot
$ModFiles       = @('xinput1_3.dll', 'ff7vr.ini')        # copied into End\Binaries\Win64
$RuntimeFiles   = @('ff7vr.log', 'ff7vr-crash-*.dmp')   # written there by the mod
$SessionFile    = 'ff7vr.session.json'
$LumaOn         = 'dxgi.dll'
$LumaOff        = 'dxgi.dll.vr-disabled'
$LogsDir        = Join-Path $Here 'logs'
$script:LastLogDir = $null

function Say([string]$msg)  { Write-Host $msg }
function Info([string]$msg) { Write-Host ("[{0}] {1}" -f (Get-Date).ToString('HH:mm:ss'), $msg) }
function Warn([string]$msg) { Write-Host ("[{0}] WARNING: {1}" -f (Get-Date).ToString('HH:mm:ss'), $msg) -ForegroundColor Yellow }
function Fail([string]$msg) { Write-Host ("[{0}] {1}" -f (Get-Date).ToString('HH:mm:ss'), $msg) -ForegroundColor Red }

function Finish([int]$code) {
    if (-not $NoPause) {
        Write-Host ''
        Write-Host 'Press any key to close this window.'
        try { [void]$Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown') } catch { }
    }
    exit $code
}

# ------------------------------------------------------------------ finding things

function Get-SteamRoot {
    try {
        $p = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath
        if ($p) { return $p.Replace('/', '\') }
    } catch { }
    return $null
}

function Get-SteamExe {
    try {
        $p = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamExe -ErrorAction Stop).SteamExe
        if ($p) { return $p.Replace('/', '\') }
    } catch { }
    $root = Get-SteamRoot
    if ($root -and (Test-Path -LiteralPath (Join-Path $root 'steam.exe'))) { return (Join-Path $root 'steam.exe') }
    return $null
}

function Find-GameRoot {
    if ($GameDir) { $cands = @($GameDir) }
    elseif ($env:FF7VR_GAME_DIR) { $cands = @($env:FF7VR_GAME_DIR) }
    else {
        $cands = @()
        $steam = Get-SteamRoot
        if ($steam) {
            $libs = @($steam)
            $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
            if (Test-Path -LiteralPath $vdf) {
                foreach ($line in Get-Content -LiteralPath $vdf) {
                    if ($line -match '^\s*"path"\s*"(.+)"\s*$') { $libs += $Matches[1].Replace('\\', '\') }
                }
            }
            foreach ($lib in @($libs | Select-Object -Unique)) {
                $acf = Join-Path $lib "steamapps\appmanifest_$AppId.acf"
                if (-not (Test-Path -LiteralPath $acf)) { continue }
                foreach ($line in Get-Content -LiteralPath $acf) {
                    if ($line -match '^\s*"installdir"\s*"(.+)"\s*$') { $cands += (Join-Path $lib ('steamapps\common\' + $Matches[1])) }
                }
            }
        }
    }
    foreach ($c in $cands) {
        if ($c -and (Test-Path -LiteralPath (Join-Path $c 'End\Binaries\Win64\ff7remake_.exe'))) { return $c }
    }
    return $null
}

function Get-GameProcs { return @(Get-Process -Name $GameProcess -ErrorAction SilentlyContinue) }

function Test-SteamRunning {
    try {
        $p = [int](Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam\ActiveProcess' -Name pid -ErrorAction Stop).pid
        if ($p -gt 0 -and (Get-Process -Id $p -ErrorAction SilentlyContinue)) { return $true }
    } catch { }
    return (@(Get-Process -Name steam -ErrorAction SilentlyContinue).Count -gt 0)
}

function Test-VirtualDesktopRunning {
    return (@(Get-Process -Name 'VirtualDesktop.Streamer' -ErrorAction SilentlyContinue).Count -gt 0)
}

function Get-Sha256([string]$path) { return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }

# ------------------------------------------------------------------ session record
# ff7vr.session.json in End\Binaries\Win64 records every change before it is
# made, so a session that never finished can always be undone:
#   state       'session' while a session runs, 'installed' after -KeepInstalled
#   lumaSetAside true once dxgi.dll was renamed to dxgi.dll.vr-disabled
#   files       the mod's files copied in (name, sha256 of what was copied)

function Read-Session([string]$bin) {
    $p = Join-Path $bin $SessionFile
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    try {
        $o = Get-Content -LiteralPath $p -Raw | ConvertFrom-Json
        $files = @()
        if ($o.PSObject.Properties['files'] -and $o.files) { foreach ($f in @($o.files)) { $files += [pscustomobject]@{ name = [string]$f.name; sha256 = [string]$f.sha256 } } }
        $luma = $false
        if ($o.PSObject.Properties['lumaSetAside']) { $luma = [bool]$o.lumaSetAside }
        $state = 'session'
        if ($o.PSObject.Properties['state']) { $state = [string]$o.state }
        $started = ''
        if ($o.PSObject.Properties['started']) { $started = [string]$o.started }
        return [pscustomobject]@{ state = $state; started = $started; lumaSetAside = $luma; files = $files }
    } catch {
        # Unreadable record: treat it as an unfinished session with every file of ours possibly present.
        return [pscustomobject]@{ state = 'session'; started = ''; lumaSetAside = $true;
                                  files = @($ModFiles | ForEach-Object { [pscustomobject]@{ name = $_; sha256 = '' } }) }
    }
}

function Write-Session([string]$bin, $session) {
    $p = Join-Path $bin $SessionFile
    $tmp = "$p.tmp"
    $json = $session | ConvertTo-Json -Depth 4
    [System.IO.File]::WriteAllText($tmp, $json, (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $tmp -Destination $p -Force
}

function Remove-WithRetry([string]$path) {
    for ($i = 0; $i -lt 20; $i++) {
        try { Remove-Item -LiteralPath $path -Force -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 500 }
    }
    throw "Could not remove $path (is the game still running?)"
}

# Moves the mod's log and crash dumps out of the game folder into logs\<stamp>\. Returns the folder or $null.
function Save-Logs([string]$bin, [string]$stamp) {
    $found = @()
    foreach ($pat in $RuntimeFiles) { $found += @(Get-ChildItem -LiteralPath $bin -Filter $pat -File -ErrorAction SilentlyContinue) }
    if ($found.Count -eq 0) { return $null }
    $dest = Join-Path $LogsDir $stamp
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    foreach ($f in $found) {
        Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $dest $f.Name) -Force
        Remove-WithRetry $f.FullName
    }
    return $dest
}

# Puts the game folder back to normal. Returns $true when it is.
function Invoke-Restore([string]$bin, [string]$why) {
    $ok = $true
    $session = Read-Session $bin
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
    if ($session -and $session.started) {
        try { $stamp = ([datetime]$session.started).ToString('yyyyMMdd-HHmmss') } catch { }
    }
    $logDir = Save-Logs $bin $stamp
    if ($logDir) { Info "Kept the mod's log in $logDir"; $script:LastLogDir = $logDir }

    if ($session) {
        Info "Removing the mod's files ($why)"
        foreach ($f in @($session.files)) {
            $p = Join-Path $bin $f.name
            if (-not (Test-Path -LiteralPath $p)) { continue }
            if ($f.sha256 -and (Get-Sha256 $p) -ne $f.sha256) { Warn "$($f.name) was changed after it was copied; removing it anyway (it belongs to the mod)" }
            try { Remove-WithRetry $p } catch { Fail "$_"; $ok = $false }
        }
    }

    # dxgi.dll: put Luma back whenever it is set aside and nothing has replaced it.
    $on = Join-Path $bin $LumaOn
    $off = Join-Path $bin $LumaOff
    $hasOn = Test-Path -LiteralPath $on
    $hasOff = Test-Path -LiteralPath $off
    if ($hasOff -and -not $hasOn) {
        try { Rename-Item -LiteralPath $off -NewName $LumaOn -ErrorAction Stop; Info 'Put ReShade/Luma (dxgi.dll) back' }
        catch { Fail "Could not rename $LumaOff back to $LumaOn`: $_"; $ok = $false }
    } elseif ($hasOff -and $hasOn) {
        Warn "Both $LumaOn and $LumaOff exist in $bin. Left both alone: decide by hand which one to keep."
        $ok = $false
    }

    if ($ok) {
        $sp = Join-Path $bin $SessionFile
        if (Test-Path -LiteralPath $sp) { Remove-WithRetry $sp }
    }
    return $ok
}

function Show-Status([string]$root, [string]$bin) {
    Say "Game folder:   $root"
    Say ("Game running:  {0}" -f $(if (@(Get-GameProcs).Count -gt 0) { 'yes' } else { 'no' }))
    $session = Read-Session $bin
    if ($session) { Say "Mod:           installed (state '$($session.state)', since $($session.started))" }
    else { Say 'Mod:           not installed' }
    foreach ($n in $ModFiles) { Say ("  {0,-14} {1}" -f $n, $(if (Test-Path -LiteralPath (Join-Path $bin $n)) { 'present' } else { 'absent' })) }
    $luma = 'not installed'
    if (Test-Path -LiteralPath (Join-Path $bin $LumaOn)) { $luma = 'in place (dxgi.dll)' }
    if (Test-Path -LiteralPath (Join-Path $bin $LumaOff)) { $luma = 'set aside (dxgi.dll.vr-disabled)' }
    Say "ReShade/Luma:  $luma"
    Say ("Steam:         {0}" -f $(if (Test-SteamRunning) { 'running' } else { 'not running' }))
    Say ("Virtual Desktop Streamer: {0}" -f $(if (Test-VirtualDesktopRunning) { 'running' } else { 'not running' }))
}

# ------------------------------------------------------------------ main

$root = Find-GameRoot
if (-not $root) {
    Fail 'FINAL FANTASY VII REMAKE INTERGRADE was not found through Steam.'
    Say  'Start this script with -GameDir "<the game folder that contains End>" or set FF7VR_GAME_DIR.'
    Finish 1
}
$bin = Join-Path $root 'End\Binaries\Win64'

if ($Action -eq 'status') { Show-Status $root $bin; Finish 0 }

if (@(Get-GameProcs).Count -gt 0) {
    if ($Action -eq 'restore') {
        Fail 'The game is running. Close it first, then run restore again.'
    } else {
        Fail 'The game is already running. Close it first, then start the VR session again.'
    }
    Finish 2
}

if ($Action -eq 'restore') {
    $session = Read-Session $bin
    $lumaAside = (Test-Path -LiteralPath (Join-Path $bin $LumaOff))
    if (-not $session -and -not $lumaAside) {
        $leftover = @()
        foreach ($pat in $RuntimeFiles) { $leftover += @(Get-ChildItem -LiteralPath $bin -Filter $pat -File -ErrorAction SilentlyContinue) }
        if ($leftover.Count -eq 0) {
            Info 'Nothing to restore: the game folder is in its normal state.'
            Finish 0
        }
    }
    if (Invoke-Restore $bin 'restore') { Info 'The game folder is back to normal.'; Finish 0 }
    Fail 'Restore did not finish; see the messages above.'
    Finish 1
}

# ---- start

Say 'ff7vr: VR session for FINAL FANTASY VII REMAKE INTERGRADE'
Info "Game folder: $root"

# An earlier session that did not finish (window closed, crash, restart) is undone first.
$prev = Read-Session $bin
if ($prev -and $prev.state -ne 'installed') {
    Warn "The previous session (started $($prev.started)) did not finish cleanly. Putting things back first."
    if (-not (Invoke-Restore $bin 'previous session')) { Fail 'Could not put the game folder back; nothing else was changed.'; Finish 1 }
} elseif (-not $prev -and (Test-Path -LiteralPath (Join-Path $bin $LumaOff)) -and -not (Test-Path -LiteralPath (Join-Path $bin $LumaOn))) {
    Warn 'ReShade/Luma (dxgi.dll) was set aside by an earlier run. Putting it back first.'
    if (-not (Invoke-Restore $bin 'earlier run')) { Finish 1 }
}

# Files that are in the way and are not ours: never overwrite them.
$prev = Read-Session $bin
foreach ($n in $ModFiles) {
    $p = Join-Path $bin $n
    $ours = $false
    if ($prev) { foreach ($f in @($prev.files)) { if ($f.name -eq $n) { $ours = $true } } }
    if ((Test-Path -LiteralPath $p) -and -not $ours) {
        Fail "$p already exists and was not put there by this launcher."
        Say  'Another mod or tool uses that file. Move it away by hand, then start again.'
        Finish 1
    }
}
if ((Test-Path -LiteralPath (Join-Path $bin $LumaOn)) -and (Test-Path -LiteralPath (Join-Path $bin $LumaOff))) {
    Fail "Both $LumaOn and $LumaOff exist in $bin. Decide by hand which one to keep, then start again."
    Finish 1
}
foreach ($n in $ModFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $Here $n))) { Fail "$n is missing next to this script ($Here)."; Finish 1 }
}

# Steam has to run: the game asks the Steam client whether it may start.
if (-not (Test-SteamRunning)) {
    Warn 'Steam is not running. The game needs it; starting Steam now.'
    $steam = Get-SteamExe
    if (-not $steam) { Fail 'steam.exe was not found. Start Steam yourself, then start the VR session again.'; Finish 1 }
    Start-Process -FilePath $steam -ArgumentList '-silent' | Out-Null
    $deadline = (Get-Date).AddSeconds(120)
    while (-not (Test-SteamRunning) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
    if (-not (Test-SteamRunning)) { Fail 'Steam did not start. Start it yourself, log in, then start the VR session again.'; Finish 1 }
    Info 'Steam started; giving it 15 seconds to log in'
    Start-Sleep -Seconds 15
}

if (-not (Test-VirtualDesktopRunning)) {
    Warn 'Virtual Desktop Streamer does not seem to be running.'
    Say  '         The game will start anyway and run flat on the monitor. The mod keeps trying to'
    Say  '         reach the headset every few seconds: start the Streamer and connect the headset'
    Say  '         in Virtual Desktop, and the game appears in the headset without a restart.'
}

# Record first, then change. Every step below is undone by restore.
$started = (Get-Date).ToString('o')
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$session = [pscustomobject]@{ tool = 'ff7vr launcher'; state = 'session'; started = $started; launcher = $Here;
                              lumaSetAside = $false; keepInstalled = [bool]$KeepInstalled; files = @() }
$files = @()
foreach ($n in $ModFiles) { $files += [pscustomobject]@{ name = $n; sha256 = (Get-Sha256 (Join-Path $Here $n)) } }
$session.files = $files
$exitCode = 0
$gameStarted = $false
try {
    if (-not $KeepLuma -and (Test-Path -LiteralPath (Join-Path $bin $LumaOn))) {
        $session.lumaSetAside = $true
        Write-Session $bin $session
        Rename-Item -LiteralPath (Join-Path $bin $LumaOn) -NewName $LumaOff
        Info 'Set ReShade/Luma (dxgi.dll) aside for this session'
    } else {
        Write-Session $bin $session
        if ($KeepLuma) { Warn 'Leaving ReShade/Luma in place (-KeepLuma). The mod has not been tested together with it.' }
    }
    # Leftover log of an earlier kept install goes to logs\ first.
    $old = Save-Logs $bin ($stamp + '-before')
    if ($old) { Info "Kept an older log in $old" }
    foreach ($f in $files) {
        $dst = Join-Path $bin $f.name
        Copy-Item -LiteralPath (Join-Path $Here $f.name) -Destination $dst -Force
        if ((Get-Sha256 $dst) -ne $f.sha256) { throw "Copying $($f.name) into the game folder failed (contents differ)" }
    }
    Info 'Installed the mod for this session (xinput1_3.dll, ff7vr.ini)'

    # Start the game executable directly. SteamAppId/SteamGameId tell the Steam API
    # which game this is, so it accepts a start that did not come from the Steam client.
    $gameArgs = '-d3d11'
    if ($ExtraArgs) { $gameArgs = "$gameArgs $ExtraArgs" }
    $env:SteamAppId = "$AppId"
    $env:SteamGameId = "$AppId"
    Info "Starting the game ($gameArgs)"
    $proc = Start-Process -FilePath (Join-Path $bin 'ff7remake_.exe') -ArgumentList $gameArgs -WorkingDirectory $bin -PassThru
    $gameStarted = $true
    Info 'Game running. Play, then quit the game as usual; this window tidies up afterwards.'
    Info 'Do not close this window while the game runs (if it happens anyway, run restore.cmd afterwards).'

    # Wait until no game process is left (5 s without one, in case it restarts itself).
    $goneSince = $null
    $t0 = Get-Date
    while ($true) {
        Start-Sleep -Seconds 2
        if (@(Get-GameProcs).Count -gt 0) { $goneSince = $null; continue }
        if (-not $goneSince) { $goneSince = Get-Date }
        if (((Get-Date) - $goneSince).TotalSeconds -ge 5) { break }
    }
    $mins = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
    Info "The game has exited (after $mins min)"
    if ($mins -lt 0.5) { Warn 'The game exited very quickly. Check the log below and send it along if this was not you.' }
} catch {
    Fail "$_"
    $exitCode = 1
} finally {
    if (@(Get-GameProcs).Count -gt 0) {
        Warn 'The game is still running, so the mod cannot be removed now. Run restore.cmd after closing the game.'
        $exitCode = 1
    } elseif ($KeepInstalled -and $exitCode -eq 0) {
        $logDir = Save-Logs $bin $stamp
        if ($logDir) { Info "Kept this session's log in $logDir"; $script:LastLogDir = $logDir }
        $session.state = 'installed'
        Write-Session $bin $session
        Info 'The mod stays installed (-KeepInstalled). The game now starts with it from Steam too. Run restore.cmd to remove it.'
    } else {
        if (Invoke-Restore $bin 'end of session') {
            Info 'The game folder is back to normal.'
        } else {
            Fail 'Could not put everything back; run restore.cmd.'
            $exitCode = 1
        }
    }
}

if ($script:LastLogDir -and $gameStarted) { Say ''; Say "Log of this session: $(Join-Path $script:LastLogDir 'ff7vr.log')" }
Finish $exitCode
