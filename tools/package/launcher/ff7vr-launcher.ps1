<#
.SYNOPSIS
  Starts FINAL FANTASY VII REMAKE INTERGRADE with the ff7vr VR mod for one session,
  and puts the game folder back to normal afterwards.

.DESCRIPTION
  start (default)
    1. Finds the game: on Steam, the Epic Games Store or the Microsoft Store /
       Xbox app (see "Finding the game" below), and warns when its exe is not
       the build the mod knows (Steam, file version 1.0.0.7).
    2. If the game (or its crash reporter or root launcher) from an earlier
       session is still running or closing, or another launcher window is still
       finishing, waits for it (up to 90 s), then refuses. Two game processes
       never run at the same time.
    3. Puts things back first if an earlier session did not finish (launcher
       window closed, crash, restart).
    4. Steam copy: checks that Steam runs (starts it if not). Warns if no VR
       runtime's server or streamer seems to run (the game then simply runs flat).
       Waits, when the previous session ended less than 90 s ago, until 90 s
       have passed (see -NoIdleWait).
    5. Checks that files can be written into End\Binaries\Win64 (a Microsoft
       Store / Xbox app folder usually refuses it; then it stops before changing
       anything and says how to unlock the folder). Sets ReShade/Luma's dxgi.dll
       aside as dxgi.dll.vr-disabled, copies the mod (xinput1_3.dll) and
       ff7vr.ini into End\Binaries\Win64, and records every change in
       End\Binaries\Win64\ff7vr.session.json.
    6. Starts the game executable (-d3d11) and waits until the game and its
       helper processes have completely exited, however the game ended. Epic and
       Microsoft Store / Xbox copies: when the executable cannot be started or
       closes again within a few seconds, the game is started through its store
       instead (the Epic Games Launcher, or the Store app's entry).
    7. Keeps the session's ff7vr.log in logs\<time>\ next to this script,
       removes the mod's files and puts dxgi.dll back.
    8. Closes its window by itself after a 10-second countdown when all went
       well; after any warning or error the window stays open until a key is
       pressed. Nothing waits for that key: everything is put back before.

  restore
    Puts the game folder back to normal: removes the mod's files and puts
    dxgi.dll back. Harmless when nothing needs doing. Waits like start for a
    game or launcher that is still finishing, then refuses while the game runs.
    Without -GameDir or FF7VR_GAME_DIR it looks at every install it finds (all
    stores), so it also tidies up a copy that FF7VR_STORE no longer names.

  status
    Shows what is in place, changes nothing.

  diagnostics
    Collects what is needed to look into a problem into one zip next to this
    script (diagnostics-<time>.zip): the last session's log folder, the
    ff7vr.ini in use, VERSION.txt, and a system.txt with Windows version,
    graphics card and driver, the OpenXR runtimes, and the state of the game
    folder. Your Windows user name and profile path are replaced by
    %USERNAME% and %USERPROFILE% in every text file of the zip. Crash dumps
    (*.dmp) are left out unless -IncludeDumps is given. Changes nothing else.

  Finding the game
    1. -GameDir, or the environment variable FF7VR_GAME_DIR: that folder.
    2. Steam: Steam's library folders (libraryfolders.vdf) and app 1462040.
    3. Epic Games Store: the Epic Games Launcher's install manifests
       (%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item).
    4. Microsoft Store / Xbox app: the user's installed packages (Get-AppxPackage)
       and the Xbox app's XboxGames folders (<drive>\XboxGames\<game>\Content).
    When the game is installed from several stores, Steam is used unless
    -Store or the environment variable FF7VR_STORE (steam, epic or xbox) names
    another. The mod has only been tested with the Steam copy.

.PARAMETER KeepInstalled
  Leave the mod (and the Luma rename, unless -KeepLuma) in place after the game
  exits, so the game can be started from its store with the mod. Undo with restore.

.PARAMETER KeepLuma
  Do not set Luma's dxgi.dll aside. With ReShade/Luma loaded the mod
  currently does not reach the headset (its D3D11 hooks are not installed,
  the game runs flat); for testing only.

.PARAMETER GameDir
  The game's install folder (the one containing End\). Found by itself when not
  given (see "Finding the game"); the environment variable FF7VR_GAME_DIR also works.

.PARAMETER Store
  steam, epic or xbox: which store's copy to use when several are installed (or
  which store a -GameDir folder belongs to). The environment variable
  FF7VR_STORE also works.

.PARAMETER ViaStore
  Epic and Microsoft Store / Xbox copies: start the game through its store at
  once instead of trying the executable first. -ExtraArgs then does not reach
  the game.

.PARAMETER IncludeDumps
  diagnostics only: also put the mod's crash dumps (*.dmp) into the zip. A dump
  holds part of the game's memory; include it only when asked for.

.PARAMETER ExtraArgs
  Extra command-line arguments for the game.

.PARAMETER NoIdleWait
  Start at once, also right after a previous session. Normally a start within
  90 s of the previous session's end waits until 90 s have passed: a game
  started again soon after quitting can stay slow for minutes.

.PARAMETER NoPause
  Close the window at once: no countdown, no key press, also after an error.

.EXAMPLE
  start-vr.cmd
  restore.cmd
  powershell -NoProfile -ExecutionPolicy Bypass -File ff7vr-launcher.ps1 start -KeepInstalled
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet('start', 'restore', 'status', 'diagnostics')]
    [string]$Action = 'start',
    [switch]$KeepInstalled,
    [switch]$KeepLuma,
    [string]$GameDir = '',
    [string]$Store = '',
    [switch]$ViaStore,
    [string]$ExtraArgs = '',
    [switch]$NoPause,
    [switch]$NoIdleWait,
    [switch]$IncludeDumps
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$AppId          = 1462040
$GameProcess    = 'ff7remake_'
$Here           = $PSScriptRoot
$ModFiles       = @('xinput1_3.dll', 'ff7vr.ini')        # copied into End\Binaries\Win64
$RuntimeFiles   = @('ff7vr.log', 'ff7vr-crash-*.dmp')   # written there by the mod
$SessionFile    = 'ff7vr.session.json'
$HeadsetFile    = 'ff7vr-headset.txt'                  # the last headset, kept next to this script between sessions
$LumaOn         = 'dxgi.dll'
$LumaOff        = 'dxgi.dll.vr-disabled'
$LogsDir        = Join-Path $Here 'logs'
$script:LastLogDir = $null

$AutoCloseSeconds = 10        # a session that ended without problems closes its window after this
$StartWaitSeconds = 90        # how long start/restore wait for a previous game or launcher to finish
$ExitWaitSeconds  = 90        # a start this soon after the previous session's end waits until this long after it
$LastExitFile     = 'last-exit.txt'   # in logs\: when the launcher saw the previous game exit
$script:Warned     = $false   # set by Warn and Fail; a session with warnings keeps its window open
$script:AutoClose  = $false   # set for a session that ended cleanly
$script:Mutex      = $null
$script:LockHeld   = $false

function Say([string]$msg)  { Write-Host $msg }
function Info([string]$msg) { Write-Host ("[{0}] {1}" -f (Get-Date).ToString('HH:mm:ss'), $msg) }
function Warn([string]$msg) { $script:Warned = $true; Write-Host ("[{0}] WARNING: {1}" -f (Get-Date).ToString('HH:mm:ss'), $msg) -ForegroundColor Yellow }
function Fail([string]$msg) { $script:Warned = $true; Write-Host ("[{0}] {1}" -f (Get-Date).ToString('HH:mm:ss'), $msg) -ForegroundColor Red }

function Release-LauncherLock {
    if ($script:Mutex) {
        try { $script:Mutex.ReleaseMutex() } catch { }
        try { $script:Mutex.Dispose() } catch { }
        $script:Mutex = $null
    }
}

# Ends the script. Everything is already put back at this point; the window only
# stays so the messages can be read. A session that ended without problems closes
# after a short countdown; anything else waits for a key.
function Finish([int]$code) {
    Release-LauncherLock
    if (-not $NoPause) {
        Write-Host ''
        $waitKey = $true
        if ($script:AutoClose -and $code -eq 0 -and -not $script:Warned) {
            $waitKey = $false
            try {
                while ($Host.UI.RawUI.KeyAvailable) { [void]$Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown') }
                for ($s = $AutoCloseSeconds; $s -gt 0; $s--) {
                    Write-Host ("`rAll done. This window closes in {0,2} s (press a key to keep it open)." -f $s) -NoNewline
                    for ($i = 0; $i -lt 10; $i++) {
                        if ($Host.UI.RawUI.KeyAvailable) {
                            [void]$Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown')
                            $waitKey = $true
                            break
                        }
                        Start-Sleep -Milliseconds 100
                    }
                    if ($waitKey) { break }
                }
            } catch { Start-Sleep -Seconds $AutoCloseSeconds }
            Write-Host ''
        }
        if ($waitKey) {
            Write-Host 'Press any key to close this window.'
            try { [void]$Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown') } catch { }
        }
    }
    exit $code
}

# ------------------------------------------------------------------ finding things

# The store lookup (Steam, Epic Games Store, Microsoft Store / Xbox app) is shared with the
# developer tools; it sits next to this script.
$StoresScript = Join-Path $PSScriptRoot 'ff7vr-stores.ps1'
if (-not (Test-Path -LiteralPath $StoresScript)) {
    Fail "ff7vr-stores.ps1 is missing next to this script ($Here). Unzip the whole package again."
    Finish 1
}
. $StoresScript

function Get-SteamExe {
    try {
        $p = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamExe -ErrorAction Stop).SteamExe
        if ($p) { return $p.Replace('/', '\') }
    } catch { }
    $root = Get-SteamRootPath
    if ($root -and (Test-Path -LiteralPath (Join-Path $root 'steam.exe'))) { return (Join-Path $root 'steam.exe') }
    return $null
}

# One line per install, for messages: "Steam: D:\...\FINAL FANTASY VII REMAKE".
function Format-Install($inst) { return "$($inst.StoreName): $($inst.Root)" }

# The game exe's build against the one the mod knows. Returns a line for status, and warns
# (once, with what to expect) when the build is another one.
function Test-GameBuild($inst, [switch]$Quiet) {
    $b = Get-ExeBuildInfo $inst.Exe
    if ($b.Error) {
        $line = "Game exe:      not readable ($($b.Error))"
    } elseif ($b.Known) {
        $line = "Game exe:      file version $($b.FileVersion), the build the mod knows"
    } else {
        $line = ("Game exe:      file version {0} (image size 0x{1:X}, time stamp 0x{2:X8}): NOT the build the mod knows" -f $b.FileVersion, $b.SizeOfImage, $b.TimeDateStamp)
    }
    if (-not $Quiet -and -not $b.Known -and -not $b.Error) {
        Warn ("This copy of the game ({0}, file version {1}) is not the build the mod knows (the Steam build, file" -f $inst.StoreName, $b.FileVersion)
        Say  '           version 1.0.0.7). The mod will not render it in 3D: the game starts and runs flat (on the'
        Say  '           monitor, at best on the flat virtual screen in the headset), and the log says'
        Say  '           "engine: stereo disabled: unknown game build".'
        Say  '           Adding allow_unknown_build = 1 under [stereo] in ff7vr.ini makes the mod try anyway; it then'
        Say  '           renders 3D only if every one of its checks passes. That has never been tried with this build.'
    }
    return $line
}

# The game and its helpers: ff7remake_.exe, the small launcher ff7remake.exe in the
# install root and the engine's crash reporter (only copies started from the game
# folder count). Returns plain records, not Process objects: a Process object keeps
# a handle to the process, and a handle kept to the exited game can make it look
# still present to other programs (VR runtimes allow one application at a time).
function Get-GameProcs {
    $out = @()
    foreach ($n in @($GameProcess, 'ff7remake', 'CrashReportClient')) {
        foreach ($p in @(Get-Process -Name $n -ErrorAction SilentlyContinue)) {
            try {
                $ours = ($n -eq $GameProcess)
                if (-not $ours -and $root) {
                    $path = $null
                    try { $path = $p.Path } catch { }
                    $ours = ($path -and $path.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase))
                }
                if ($ours) { $out += [pscustomobject]@{ Name = $p.ProcessName; Id = $p.Id } }
            } finally { $p.Dispose() }
        }
    }
    return $out
}

function Format-Procs($procs) { return ((@($procs) | ForEach-Object { "$($_.Name).exe (pid $($_.Id))" }) -join ', ') }

# Takes the launcher lock: only one launcher works on the game folder at a time.
# Returns $true when taken, $false when another launcher still holds it.
function Enter-LauncherLock([int]$timeoutMs) {
    if (-not $script:Mutex) { $script:Mutex = New-Object System.Threading.Mutex($false, 'Local\ff7vr-launcher') }
    try { return $script:Mutex.WaitOne($timeoutMs) }
    catch [System.Threading.AbandonedMutexException] { return $true }   # its holder was closed or killed: ours now
}

# Waits until no other launcher is busy and no game process (or helper) is left,
# up to $StartWaitSeconds. Returns $true when the way is clear; the lock is then held.
function Wait-ClearToStart([string]$what) {
    $deadline = (Get-Date).AddSeconds($StartWaitSeconds)
    $said = ''
    while ($true) {
        if (-not $script:Mutex -or -not $script:LockHeld) {
            $script:LockHeld = Enter-LauncherLock 0
        }
        $procs = @(Get-GameProcs)
        if ($script:LockHeld -and $procs.Count -eq 0) {
            if ($said) { Info 'Done waiting.' }
            return $true
        }
        if (-not $script:LockHeld) { $msg = 'Another launcher window is still finishing a session. Waiting for it to finish ...' }
        else { $msg = "The game from an earlier session is still running or closing ($(Format-Procs $procs)). Waiting for it to exit ..." }
        if ($msg -ne $said) { Info $msg; $said = $msg }
        if ((Get-Date) -ge $deadline) { break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $script:LockHeld) {
        Fail "Another launcher window is still busy after $StartWaitSeconds s. Let it finish (or close it), then $what again."
    } else {
        $procs = @(Get-GameProcs)
        Fail "The game is still running after $StartWaitSeconds s: $(Format-Procs $procs)."
        if (@($procs | Where-Object { $_.Name -eq 'CrashReportClient' }).Count -gt 0) {
            Say 'The game crashed and its crash report window is open: close that window, then try again.'
        } else {
            Say "Quit the game (or end it in Task Manager), then $what again."
        }
    }
    return $false
}

function Test-SteamRunning {
    try {
        $p = [int](Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam\ActiveProcess' -Name pid -ErrorAction Stop).pid
        if ($p -gt 0 -and (Get-Process -Id $p -ErrorAction SilentlyContinue)) { return $true }
    } catch { }
    return (@(Get-Process -Name steam -ErrorAction SilentlyContinue).Count -gt 0)
}

# Processes that show a VR runtime is up: the same list the mod uses to order the
# runtimes it tries ([xr] runtime = auto). Process names without .exe.
$VrRuntimeProcesses = [ordered]@{
    'VirtualDesktop.Streamer' = 'Virtual Desktop Streamer'
    'vrserver'                = 'SteamVR'
    'OVRServer_x64'           = 'Meta Quest Link (Oculus)'
    'PICO Connect'            = 'PICO Connect'
    'PICO Connect TMP'        = 'PICO Connect'
    'MixedRealityPortal'      = 'Windows Mixed Reality'
}

# Names of the VR runtimes whose process runs (empty: none).
function Get-RunningVrRuntimes {
    $found = @()
    foreach ($p in $VrRuntimeProcesses.Keys) {
        if (@(Get-Process -Name $p -ErrorAction SilentlyContinue).Count -gt 0) {
            $n = $VrRuntimeProcesses[$p]
            if ($found -notcontains $n) { $found += $n }
        }
    }
    return , $found
}

function Get-Sha256([string]$path) { return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }

# The package's version, the first line of VERSION.txt ("ff7vr v1.2 (abc1234)"), or 'ff7vr'.
function Get-PackageVersion {
    try {
        $l = (Get-Content -LiteralPath (Join-Path $Here 'VERSION.txt') -TotalCount 1 -ErrorAction Stop).Trim()
        if ($l) { return $l }
    } catch { }
    return 'ff7vr'
}

# ---- the graphics card after a game exit
# A game started again soon after the previous one's exit can stay slow for minutes: about 10
# frames per second from the load on, the graphics card busy at a third of its power. In the
# measurements it happened to 2 of 5 games started 9 s after an exit and to none of 5 started
# 90 s after. The start therefore waits until 90 s have passed since the previous session ended.

# When the previous session ended, or $null: the launcher's own note of the last exit, and the
# time of the newest log of the mod (written at least every 10 s while the game runs), in the
# game folder (installed by hand), its ff7vr-logs\ folder and this launcher's logs\ folder.
function Get-LastGameExit([string]$bin) {
    $times = @()
    $note = Join-Path $LogsDir $LastExitFile
    if (Test-Path -LiteralPath $note) {
        try { $times += [datetime]::Parse((Get-Content -LiteralPath $note -TotalCount 1).Trim(), $null, [System.Globalization.DateTimeStyles]::RoundtripKind) } catch { }
    }
    $log = Join-Path $bin 'ff7vr.log'
    if (Test-Path -LiteralPath $log) { $times += (Get-Item -LiteralPath $log).LastWriteTime }
    foreach ($dir in @((Join-Path $bin 'ff7vr-logs'), $LogsDir)) {
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        $f = @(Get-ChildItem -LiteralPath $dir -Filter '*.log' -File -Recurse -Depth 1 -ErrorAction SilentlyContinue |
               Sort-Object LastWriteTime -Descending | Select-Object -First 1)
        if ($f.Count -gt 0) { $times += $f[0].LastWriteTime }
    }
    if ($times.Count -eq 0) { return $null }
    return ($times | Sort-Object -Descending | Select-Object -First 1)
}

function Save-LastGameExit([datetime]$when) {
    try {
        New-Item -ItemType Directory -Force -Path $LogsDir | Out-Null
        Set-Content -LiteralPath (Join-Path $LogsDir $LastExitFile) -Value $when.ToString('o') -Encoding ASCII
    } catch { }
}

function Wait-AfterGameExit([string]$bin) {
    $last = Get-LastGameExit $bin
    if (-not $last) { return }
    $since = ((Get-Date) - $last).TotalSeconds
    if ($since -lt 0 -or $since -ge $ExitWaitSeconds) { return }
    $wait = $ExitWaitSeconds - $since
    Info ("The previous session ended {0:N0} s ago. Waiting {1:N0} s before starting the game:" -f $since, $wait)
    Say  '           a game started again soon after quitting can stay slow (about 10 frames per second) for minutes.'
    Start-Sleep -Milliseconds ([int]($wait * 1000))
    Info 'Done waiting.'
}

# ------------------------------------------------------------------ starting the game

# An Epic or Store copy that has to be started through its store closes again soon after a
# direct start, perhaps after a message box the player has to close first.
$QuickExitSeconds        = 60    # a close within this long after a direct start: start it through the store
$RestartGraceSeconds     = 10    # after such a close, how long to wait for the game starting itself again
$EpicRestartGraceSeconds = 60    # the same when the close started the Epic Games Launcher
$StoreStartSeconds       = 180   # how long a start through the store may take until the game process appears

# Starts ff7remake_.exe and returns its process id. No handle to the game is kept (see Get-GameProcs).
function Start-GameExe([string]$gameArgs) {
    $proc = Start-Process -FilePath $inst.Exe -ArgumentList $gameArgs -WorkingDirectory $inst.Bin -PassThru
    $id = $proc.Id
    $proc.Dispose()
    return $id
}

# The process id of a running ff7remake_.exe, or $null.
function Get-GamePid {
    $p = @(Get-GameProcs | Where-Object { $_.Name -eq $GameProcess })
    if ($p.Count -gt 0) { return $p[0].Id }
    return $null
}

function Wait-GamePid([int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ($true) {
        $id = Get-GamePid
        if ($id) { return $id }
        if ((Get-Date) -ge $deadline) { return $null }
        Start-Sleep -Milliseconds 500
    }
}

# Epic and Microsoft Store / Xbox copies. First the executable itself (the mod adds
# -d3d11 by itself either way); if it cannot be started or closes again at once, the
# store is asked to start the game. Returns the game's process id or $null.
function Start-StoreGame($inst, [string]$gameArgs) {
    $storeName = $inst.StoreName
    if (-not $ViaStore) {
        $epicBefore = @(Get-Process -Name 'EpicGamesLauncher' -ErrorAction SilentlyContinue).Count -gt 0
        Info "Starting the game ($gameArgs)"
        $id = $null
        try { $id = Start-GameExe $gameArgs } catch { Info "The game's executable could not be started directly ($($_.Exception.Message))." }
        if ($id) {
            Info "If a message asks you to start the game through the $storeName, close it: this window then does that."
            $deadline = (Get-Date).AddSeconds($QuickExitSeconds)
            while ((Get-Date) -lt $deadline -and (Get-GamePid)) { Start-Sleep -Milliseconds 500 }
            $now = Get-GamePid
            if ($now) { return $now }
            Info "The game closed again soon after starting; it probably has to be started through the $storeName."
            # It may be starting itself again through its store: give that time before asking the store.
            $grace = $RestartGraceSeconds
            if ($inst.Store -eq 'epic' -and -not $epicBefore -and @(Get-Process -Name 'EpicGamesLauncher' -ErrorAction SilentlyContinue).Count -gt 0) { $grace = $EpicRestartGraceSeconds }
            $again = Wait-GamePid $grace
            if ($again) { Info "The game was started again through its store (pid $again)."; return $again }
        }
    }
    $left = @(Get-GameProcs)
    if ($left.Count -gt 0) {
        Fail "Not starting the game through the $storeName while $(Format-Procs $left) is still running."
        return $null
    }
    if ($ExtraArgs) { Warn '-ExtraArgs does not reach the game when the store starts it.' }
    $target = $null
    if ($inst.Store -eq 'epic') { $target = Get-EpicLaunchUri $inst } else { $target = Get-XboxLaunchTarget $inst }
    if ($target) {
        Info "Starting the game through the $storeName ($target)"
        try {
            if ($inst.Store -eq 'epic') { Start-Process -FilePath $target | Out-Null }
            else { Start-Process -FilePath 'explorer.exe' -ArgumentList $target | Out-Null }
        } catch {
            Warn "The $storeName could not be asked to start the game: $($_.Exception.Message)"
            $target = $null
        }
    }
    if (-not $target) {
        Say "           Start the game yourself now from the $storeName; this window waits $StoreStartSeconds s for it."
    } elseif ($inst.Store -eq 'epic') {
        Say '           If the Epic Games Launcher asks you to log in, do so; the game then starts.'
    }
    $id = Wait-GamePid $StoreStartSeconds
    if (-not $id) { Fail "The game did not start within $StoreStartSeconds s." }
    return $id
}

# ------------------------------------------------------------------ session record
# ff7vr.session.json in End\Binaries\Win64 records every change before it is
# made, so a session that never finished can always be undone:
#   state       'session' while a session runs, 'installed' after -KeepInstalled
#   lumaSetAside true once dxgi.dll was renamed to dxgi.dll.vr-disabled
#   files       the mod's files copied in (name, sha256 of what was copied)
# Names read from the record are only acted on when they are in $ModFiles.

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
        return [pscustomobject]@{ state = $state; started = $started; lumaSetAside = $luma; files = $files; unreadable = $false }
    } catch {
        # Unreadable record: treat it as an unfinished session with every file of ours possibly present;
        # restore then only removes a file that is identical to this package's own copy.
        return [pscustomobject]@{ state = 'session'; started = ''; lumaSetAside = $true; unreadable = $true;
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

# A name from the session record is acted on only when it is one of the mod's own files
# (a plain file name, no folder part).
function Test-ModFileName([string]$name) {
    if (-not $name -or $name -match '[\\/:]' -or $name.Trim('.') -eq '') { return $false }
    return ($ModFiles -contains $name)
}

# Removes one of the mod's files from the game folder. Returns $true when it is gone.
#   unchanged since it was copied  -> removed
#   ff7vr.ini changed (settings saved during the session) -> copied back next to this script as the
#                                     player's settings (the earlier copy kept as ff7vr.ini.previous), removed
#   anything else changed, or no record of what was copied -> removed only when identical to this
#                                     package's own copy, otherwise left in place with a warning
function Remove-ModFile([string]$bin, [string]$name, [string]$recorded, [bool]$unreadable) {
    $p = Join-Path $bin $name
    $cur = Get-Sha256 $p
    $known = (-not $unreadable) -and [bool]$recorded
    $pkgCopy = Join-Path $Here $name
    $pkgHash = $null
    if (Test-Path -LiteralPath $pkgCopy) { $pkgHash = Get-Sha256 $pkgCopy }

    if ($known -and $cur -eq $recorded) {
        # as copied
    } elseif ($known -and $name -eq 'ff7vr.ini') {
        if ($cur -ne $pkgHash) {
            try {
                if ($pkgHash) { Copy-Item -LiteralPath $pkgCopy -Destination (Join-Path $Here 'ff7vr.ini.previous') -Force -ErrorAction Stop }
                Copy-Item -LiteralPath $p -Destination $pkgCopy -Force -ErrorAction Stop
            } catch {
                Fail "ff7vr.ini in the game folder holds settings changed during the session, and copying it to $pkgCopy failed: $_"
                Say  "           It was left in $bin. Copy it next to start-vr.cmd by hand, then run restore.cmd again."
                return $false
            }
            Info "Kept the settings changed during the session: $pkgCopy (the one before is ff7vr.ini.previous)"
        }
    } elseif ($pkgHash -and $cur -eq $pkgHash) {
        # identical to this package's copy
    } else {
        $why = $(if ($known) { 'it changed after the launcher copied it' } else { "$SessionFile could not be read" })
        Warn "Left $p in place: $why, and it differs from this package's own $name."
        if ($name -eq 'ff7vr.ini') {
            Say  '           If it holds settings you want to keep, copy it next to start-vr.cmd. Then delete it from the'
            Say  '           game folder by hand and run restore.cmd again.'
        } else {
            Say  '           It may belong to another mod or tool, or to another version of this mod. If you installed'
            Say  "           nothing else that uses $name, delete it by hand; otherwise leave it there. Then run"
            Say  '           restore.cmd again.'
        }
        return $false
    }
    try { Remove-WithRetry $p; return $true } catch { Fail "$_"; return $false }
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
            if (-not (Test-ModFileName $f.name)) {
                Warn "Skipped '$($f.name)' named in $SessionFile`: not one of the mod's files ($($ModFiles -join ', '))."
                continue
            }
            $p = Join-Path $bin $f.name
            if (-not (Test-Path -LiteralPath $p)) { continue }
            if (-not (Remove-ModFile $bin $f.name $f.sha256 $session.unreadable)) { $ok = $false }
        }
        # The settings panel keeps the previous ff7vr.ini as ff7vr.ini.bak when it saves; the
        # settings themselves are kept above, so the backup is not needed in the game folder.
        $bak = Join-Path $bin 'ff7vr.ini.bak'
        if (Test-Path -LiteralPath $bak) { try { Remove-WithRetry $bak } catch { Warn "Could not remove $bak`: $_" } }
        # The last headset's name (written only when ff7vr.ini has [headset:...] sections) goes
        # back to the package folder, so the next session starts with that headset's settings.
        $hs = Join-Path $bin $HeadsetFile
        if (Test-Path -LiteralPath $hs) {
            try { Copy-Item -LiteralPath $hs -Destination (Join-Path $Here $HeadsetFile) -Force -ErrorAction Stop; Remove-WithRetry $hs }
            catch { Warn "Could not move $hs to the package folder: $_" }
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

function Get-StatusLines($inst) {
    $root = $inst.Root
    $bin = $inst.Bin
    $out = @()
    $out += "Game folder:   $root"
    $out += "Store:         $($inst.StoreName) (found through $($inst.Source))"
    $out += Test-GameBuild $inst -Quiet
    $out += ("Game running:  {0}" -f $(if (@(Get-GameProcs).Count -gt 0) { 'yes' } else { 'no' }))
    $session = Read-Session $bin
    if ($session) { $out += "Mod:           installed (state '$($session.state)', since $($session.started))" }
    else { $out += 'Mod:           not installed' }
    foreach ($n in $ModFiles) { $out += ("  {0,-14} {1}" -f $n, $(if (Test-Path -LiteralPath (Join-Path $bin $n)) { 'present' } else { 'absent' })) }
    $luma = 'not installed'
    if (Test-Path -LiteralPath (Join-Path $bin $LumaOn)) { $luma = 'in place (dxgi.dll)' }
    if (Test-Path -LiteralPath (Join-Path $bin $LumaOff)) { $luma = 'set aside (dxgi.dll.vr-disabled)' }
    $out += "ReShade/Luma:  $luma"
    if ($inst.Store -eq 'steam') { $out += ("Steam:         {0}" -f $(if (Test-SteamRunning) { 'running' } else { 'not running' })) }
    $vr = Get-RunningVrRuntimes
    $out += ("VR runtimes:   {0}" -f $(if ($vr.Count) { ($vr -join ', ') + ' running' } else { 'none seen running' }))
    return $out
}

function Show-Status($inst, $others) {
    foreach ($l in (Get-StatusLines $inst)) { Say $l }
    foreach ($o in @($others)) { Say "Also installed: $(Format-Install $o) (FF7VR_STORE=$($o.Store) uses it)" }
}

# ------------------------------------------------------------------ diagnostics

function Get-SystemLines($inst, [string]$logFile) {
    $out = @()
    $out += "Collected: $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))"
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        $out += "Windows:   $($os.Caption) $($os.Version) (build $($os.BuildNumber)), $([math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GB RAM"
    } catch { $out += "Windows:   (not readable: $_)" }
    try { foreach ($c in @(Get-CimInstance Win32_Processor)) { $out += "CPU:       $($c.Name.Trim())" } } catch { }
    try {
        foreach ($g in @(Get-CimInstance Win32_VideoController)) {
            $out += "GPU:       $($g.Name), driver $($g.DriverVersion) ($($g.DriverDate)), $($g.CurrentHorizontalResolution)x$($g.CurrentVerticalResolution) $($g.CurrentRefreshRate) Hz"
        }
    } catch { $out += "GPU:       (not readable: $_)" }
    $out += "PowerShell: $($PSVersionTable.PSVersion)"
    $out += ''
    $out += 'OpenXR runtimes (with [xr] runtime = auto the mod tries the running ones, then the PC default, then the rest; its log names the one it chose):'
    try {
        $k = Get-ItemProperty -Path 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -ErrorAction Stop
        $out += "  PC default:  $($k.ActiveRuntime)"
    } catch { $out += '  PC default:  none set' }
    try {
        $av = Get-Item -Path 'HKLM:\SOFTWARE\Khronos\OpenXR\1\AvailableRuntimes' -ErrorAction Stop
        foreach ($n in $av.GetValueNames()) { $out += ("  installed:   {0} ({1})" -f $n, $(if (Test-Path -LiteralPath $n) { 'file present' } else { 'file missing' })) }
    } catch { }
    $out += ''
    if ($inst) {
        $out += Get-StatusLines $inst
    } else {
        $out += 'Game folder: not found (looked on Steam, the Epic Games Store and the Microsoft Store / Xbox app)'
    }
    if ($logFile -and (Test-Path -LiteralPath $logFile)) {
        $out += ''
        $out += "From the log ($(Split-Path -Leaf (Split-Path -Parent $logFile))\$(Split-Path -Leaf $logFile)):"
        $pat = 'ff7vr .* loaded|xr: thread started|xr: session created|xr: no session|runtime .* from now on|stereo: rendering|engine: stereo|foveation: (preset|variable|not)|CRASH|minidump written|exception 0x| ERROR '
        $hits = @(Select-String -LiteralPath $logFile -Pattern $pat -CaseSensitive | Where-Object { $_.Line -notmatch 'config: ' } | Select-Object -First 40)
        foreach ($h in $hits) { $out += "  $($h.Line)" }
        if ($hits.Count -eq 0) { $out += '  (no matching lines)' }
    }
    # Paths in the user's own profile are shown relative to it.
    if ($env:USERPROFILE) { $out = @($out | ForEach-Object { $_.Replace($env:USERPROFILE, '%USERPROFILE%') }) }
    return $out
}

# Replaces the user's profile path (as written in paths, in JSON with doubled backslashes, and with
# forward slashes) by %USERPROFILE% and the user name as a word by %USERNAME%.
function Protect-Text([string]$text) {
    $ic = [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
    if ($env:USERPROFILE) {
        $up = $env:USERPROFILE.TrimEnd('\')
        foreach ($form in @($up.Replace('\', '\\'), $up, $up.Replace('\', '/'))) {
            $text = [regex]::Replace($text, [regex]::Escape($form), '%USERPROFILE%', $ic)
        }
    }
    if ($env:USERNAME) {
        $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])' + [regex]::Escape($env:USERNAME) + '(?![A-Za-z0-9_])', '%USERNAME%', $ic)
    }
    return $text
}

# Scrubs one file in place with Protect-Text. Returns $false (file untouched) when it is not text.
function Protect-TextFile([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    if ([Array]::IndexOf($bytes, [byte]0) -ge 0) { return $false }
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    [System.IO.File]::WriteAllText($path, (Protect-Text $text), (New-Object System.Text.UTF8Encoding($false)))
    return $true
}

function Invoke-Diagnostics($inst) {
    $bin = $null
    if ($inst) { $bin = $inst.Bin }
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $stage = Join-Path ([System.IO.Path]::GetTempPath()) "ff7vr-diagnostics-$stamp"
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    try {
        $logFile = $null
        # A session that is still running or never finished has its log in the game folder.
        if ($bin -and (Test-Path -LiteralPath (Join-Path $bin 'ff7vr.log'))) {
            $d = Join-Path $stage 'game-folder'
            New-Item -ItemType Directory -Force -Path $d | Out-Null
            foreach ($pat in $RuntimeFiles + @('ff7vr.ini', $SessionFile)) {
                Get-ChildItem -LiteralPath $bin -Filter $pat -File -ErrorAction SilentlyContinue | Copy-Item -Destination $d
            }
            $logFile = Join-Path $d 'ff7vr.log'
            Info "Included the log in the game folder (a session is running or did not finish)"
        }
        $last = $null
        if (Test-Path -LiteralPath $LogsDir) {
            $last = Get-ChildItem -LiteralPath $LogsDir -Directory | Sort-Object Name -Descending | Select-Object -First 1
        }
        if ($last) {
            Copy-Item -LiteralPath $last.FullName -Destination (Join-Path $stage "logs-$($last.Name)") -Recurse
            if (-not ($logFile -and (Test-Path -LiteralPath $logFile))) { $logFile = Join-Path $last.FullName 'ff7vr.log' }
            Info "Included the last session's log folder: logs\$($last.Name)"
        } elseif (-not $logFile) {
            Warn 'No session log found (no session has run from this folder yet).'
        }
        foreach ($n in @('ff7vr.ini', 'VERSION.txt')) {
            $p = Join-Path $Here $n
            if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $stage }
        }
        [System.IO.File]::WriteAllLines((Join-Path $stage 'system.txt'), [string[]](Get-SystemLines $inst $logFile))

        # Crash dumps stay out unless asked for; every other file must be text and has the user
        # name and profile path replaced. Anything else is left out.
        $dumps = 0
        $included = @()
        foreach ($f in @(Get-ChildItem -LiteralPath $stage -File -Recurse)) {
            $rel = $f.FullName.Substring($stage.Length).TrimStart('\')
            if ($f.Extension -eq '.dmp') {
                if ($IncludeDumps) { $included += "$rel (crash dump)" } else { Remove-Item -LiteralPath $f.FullName -Force; $dumps++ }
                continue
            }
            if (-not (Protect-TextFile $f.FullName)) {
                Remove-Item -LiteralPath $f.FullName -Force
                Info "Left out $rel (not a text file)"
                continue
            }
            $included += $rel
        }
        $zip = Join-Path $Here "diagnostics-$stamp.zip"
        Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
        Info "Diagnostics collected: $zip"
        Say  'It contains:'
        foreach ($r in $included) { Say "  $r" }
        if ($IncludeDumps) {
            Say 'Crash dumps are included (-IncludeDumps).'
        } else {
            Say ("Crash dumps (*.dmp) are left out{0}; add -IncludeDumps only when asked for one." -f $(if ($dumps) { " ($dumps found)" } else { '' }))
        }
        Say  'Your Windows user name and profile folder are replaced by %USERNAME% and %USERPROFILE% in every file.'
        Say  'Send the zip along with a sentence on what happened and when; open it first to check what it holds.'
        return $true
    } catch {
        Fail "Collecting diagnostics failed: $_"
        return $false
    } finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ------------------------------------------------------------------ main

$found = Resolve-GameInstall $GameDir $Store
$inst = $found.Install
$root = $null
$bin = $null
if ($inst) { $root = $inst.Root; $bin = $inst.Bin }
$others = @()
if ($inst) { $others = @($found.All | Where-Object { $_.Exe -ne $inst.Exe }) }

if ($Action -eq 'diagnostics') {
    if (Invoke-Diagnostics $inst) { Finish 0 }
    Finish 1
}
if (-not $inst) {
    Fail $found.Error
    foreach ($e in @($script:StoreLookupErrors)) { Say "           Looking on $e" }
    foreach ($o in @($found.All)) { Say "           Found: $(Format-Install $o)" }
    Say  'Start this script with -GameDir "<the game folder that contains End>" or set FF7VR_GAME_DIR;'
    Say  'FF7VR_STORE (steam, epic or xbox) says which store the copy is from.'
    Finish 1
}

if ($Action -eq 'status') { Show-Status $inst $others; Finish 0 }

# Never two game processes at once (a VR runtime serves one application at a time,
# even while the previous one is still shutting down), and never two launchers
# working on the game folder at once.
if ($Action -eq 'restore') { $what = 'run restore' } else { $what = 'start the VR session'; Say "$(Get-PackageVersion): VR session for FINAL FANTASY VII REMAKE INTERGRADE" }
if (-not (Wait-ClearToStart $what)) { Finish 2 }

if ($Action -eq 'restore') {
    # Every install found (all stores) that has something to undo; a folder given by hand alone.
    $todo = @()
    foreach ($t in @(@($inst) + $others)) {
        $b = $t.Bin
        $need = [bool](Read-Session $b) -or (Test-Path -LiteralPath (Join-Path $b $LumaOff))
        if (-not $need) {
            foreach ($pat in $RuntimeFiles) { if (@(Get-ChildItem -LiteralPath $b -Filter $pat -File -ErrorAction SilentlyContinue).Count -gt 0) { $need = $true } }
        }
        if ($need) { $todo += $t }
    }
    if ($todo.Count -eq 0) {
        Info 'Nothing to restore: the game folder is in its normal state.'
        Finish 0
    }
    $ok = $true
    foreach ($t in $todo) {
        Info "Game folder: $(Format-Install $t)"
        if (Invoke-Restore $t.Bin 'restore') { Info 'The game folder is back to normal.' } else { $ok = $false }
    }
    if ($ok) { Finish 0 }
    Fail 'Restore did not finish; see the messages above.'
    Finish 1
}

# ---- start

Info "Game folder: $root"
Info "Store:       $($inst.StoreName) (found through $($inst.Source))"
if ($others.Count -gt 0) {
    foreach ($o in $others) { Info "Also installed: $(Format-Install $o) (FF7VR_STORE=$($o.Store) uses that one)" }
}
[void](Test-GameBuild $inst)

# The mod's files go into the game's own folder; a Microsoft Store / Xbox app folder is
# usually locked. Find out before changing anything.
if (-not (Test-FolderWritable $bin)) {
    Fail "Files cannot be written into the game folder: $bin"
    if ($inst.Store -eq 'xbox') {
        Say  '           Microsoft Store / Xbox app games are installed in a protected folder. To unlock it: open the'
        Say  '           Xbox app, go to the game''s page, then Manage (the "..." button), Files, and choose "Enable'
        Say  '           advanced management features". Then start the VR session again. Nothing was changed.'
    } else {
        Say  '           Check that the folder is not read-only and that your Windows account may change files in it,'
        Say  '           then start the VR session again. Nothing was changed.'
    }
    Finish 1
}

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

# Steam copy: Steam has to run, the game asks the Steam client whether it may start.
if ($inst.Store -eq 'steam' -and -not (Test-SteamRunning)) {
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

if ((Get-RunningVrRuntimes).Count -eq 0) {
    Warn 'No VR runtime seems to be running.'
    Say  ('         Looked for: ' + ((@($VrRuntimeProcesses.Values) | Select-Object -Unique) -join ', ') + '.')
    Say  '         The game will start anyway and run flat on the monitor. The mod keeps trying to'
    Say  '         reach a headset every few seconds: start your headset''s PC app (Virtual Desktop'
    Say  '         Streamer, SteamVR, Meta Quest Link, ...) and connect the headset, and the game'
    Say  '         appears in it without a restart.'
}

if (-not $NoIdleWait) { Wait-AfterGameExit $bin }

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
        if ($KeepLuma) {
            Warn 'Leaving ReShade/Luma in place (-KeepLuma). With it loaded the mod currently does not reach'
            Warn 'the headset: the game runs flat on the monitor. Kept for testing only.'
        }
    }
    # Leftover log of an earlier kept install goes to logs\ first.
    $old = Save-Logs $bin ($stamp + '-before')
    if ($old) { Info "Kept an older log in $old" }
    foreach ($f in $files) {
        $dst = Join-Path $bin $f.name
        Copy-Item -LiteralPath (Join-Path $Here $f.name) -Destination $dst -Force
        if ((Get-Sha256 $dst) -ne $f.sha256) { throw "Copying $($f.name) into the game folder failed (contents differ)" }
    }
    $hsKept = Join-Path $Here $HeadsetFile
    if (Test-Path -LiteralPath $hsKept) { Copy-Item -LiteralPath $hsKept -Destination (Join-Path $bin $HeadsetFile) -Force }
    Info 'Installed the mod for this session (xinput1_3.dll, ff7vr.ini)'

    $gameArgs = '-d3d11'
    if ($ExtraArgs) { $gameArgs = "$gameArgs $ExtraArgs" }
    if ($inst.Store -eq 'steam') {
        # Start the game executable directly. SteamAppId/SteamGameId tell the Steam API
        # which game this is, so it accepts a start that did not come from the Steam client.
        $env:SteamAppId = "$AppId"
        $env:SteamGameId = "$AppId"
        Info "Starting the game ($gameArgs)"
        $gamePid = Start-GameExe $gameArgs
    } else {
        $gamePid = Start-StoreGame $inst $gameArgs
        if (-not $gamePid) { throw 'The game did not start; see the messages above.' }
    }
    $gameStarted = $true
    $script:Warned = $false   # from here on, any warning keeps the window open at the end
    Info "Game running (pid $gamePid). Play, then quit the game as usual; this window tidies up and closes by itself."
    Info 'Do not close this window while the game runs (if it happens anyway, run restore.cmd afterwards).'

    # Wait until the game and its helpers are completely gone (5 s without any, in
    # case the game restarts itself), however it ended: quit, Alt+F4, killed, crash.
    $goneSince = $null
    $t0 = Get-Date
    $said = ''
    while ($true) {
        Start-Sleep -Seconds 1
        $procs = @(Get-GameProcs)
        if ($procs.Count -gt 0) {
            $goneSince = $null
            if (@($procs | Where-Object { $_.Name -eq $GameProcess }).Count -eq 0) {
                $msg = "The game has closed; waiting for $(Format-Procs $procs) to exit."
                if (@($procs | Where-Object { $_.Name -eq 'CrashReportClient' }).Count -gt 0) {
                    $msg += ' The game crashed: close the crash report window to finish.'
                }
                if ($msg -ne $said) { Info $msg; $said = $msg }
            }
            continue
        }
        if (-not $goneSince) { $goneSince = Get-Date }
        if (((Get-Date) - $goneSince).TotalSeconds -ge 5) { break }
    }
    Save-LastGameExit $goneSince
    $mins = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
    Info "The game has exited completely (after $mins min)"
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
        Info "The mod stays installed (-KeepInstalled). The game now starts with it from $($inst.StoreName) too. Run restore.cmd to remove it."
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
if ($exitCode -eq 0 -and -not $script:Warned) { Info 'Session finished. You can start the next one now (it waits until 90 s after this exit before the game starts).' }
$script:AutoClose = $gameStarted
Finish $exitCode
