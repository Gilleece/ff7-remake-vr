<#
.SYNOPSIS
  Run the game with the mod, unattended, and clean up afterwards.

.DESCRIPTION
  1. Takes the game lock (.locks\game), waiting up to -LockWaitSeconds.
  2. Backs up the save folder if no verified backup exists yet (backup-saves.ps1).
  3. Renames Luma/ReShade's dxgi.dll to dxgi.dll.vr-disabled (not with -KeepLuma).
  4. Deploys the mod from the build directory (deploy.ps1), unless -NoMod.
  5. Starts End\Binaries\Win64\ff7remake_.exe directly with -d3d11, windowed
     at -Width x -Height unless -Fullscreen (see docs\dev-harness.md for why
     the exe is started directly and not through Steam).
  6. Waits for the game window and for "ff7vr: initialised" in ff7vr.log.
  7. Optionally waits for a screen (-Until title|gameplay), a regex in
     ff7vr.log (-WaitLog), and/or a number of seconds (-WaitSeconds).
  8. Optionally saves a PNG of the game window (-Screenshot).
  9. Unless -KeepRunning: stops the game, undeploys (the log and any crash
     dumps go to captures\runs\<time>\), restores dxgi.dll, releases the lock.
     With -KeepRunning, run stop.ps1 when done.

  Exit code 0 when every requested step succeeded. Environment variables for
  the game can be passed with -GameEnv NAME=value[;NAME2=value2] (they reach the game because
  it is started directly).

.EXAMPLE
  # Thin end-to-end check: start, reach the title screen, capture, stop.
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\launch.ps1 -Until title -Screenshot

  # Load the latest save, capture gameplay, keep the game running.
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\launch.ps1 -Until gameplay -Screenshot -KeepRunning

  # Vanilla game (no mod) for comparison, with an OpenXR runtime override.
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\launch.ps1 -NoMod -Until title
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\launch.ps1 -GameEnv "XR_RUNTIME_JSON=C:\path\to\runtime.json"
#>
param(
    [string]$BuildDir = '',
    [string]$Ini = '',
    [ValidateSet('none', 'title', 'gameplay')]
    [string]$Until = 'none',
    [string]$WaitLog = '',
    [int]$WaitSeconds = 0,
    [switch]$Screenshot,
    [string]$ScreenshotPath = '',
    [switch]$KeepRunning,
    [switch]$NoMod,
    [switch]$KeepLuma,
    [switch]$Fullscreen,
    [int]$Width = 1280,
    [int]$Height = 720,
    [string]$ExtraArgs = '',
    [string[]]$GameEnv = @(),
    [ValidateSet('direct', 'steam')]
    [string]$Via = 'direct',
    [int]$StartTimeout = 120,
    [int]$UntilTimeout = 240,
    [int]$LockWaitSeconds = 900
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$owner = Get-DefaultOwner
$runStamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$failures = @()
$startedLock = $false

$preLock = Get-LockInfo
$alreadyHeld = ($null -ne $preLock -and $preLock.Owner -eq $owner)
if (-not (Lock-Game -owner $owner -waitSeconds $LockWaitSeconds)) { Write-Step 'Could not get the game lock'; exit 3 }
if (@(Get-GameProcesses).Count -gt 0) {
    # Not ours to kill: someone may be running the game by hand. stop.ps1 cleans up a run of our own.
    if (-not $alreadyHeld) { [void](Unlock-Game -owner $owner) }
    Write-Step 'ff7remake_ is already running. Close it or run tools\dev\stop.ps1 first.'
    exit 3
}
$startedLock = $true

try {
    [void](Backup-Saves -ifMissing)

    if ($KeepLuma) { Write-Step 'Luma/ReShade dxgi.dll left enabled (-KeepLuma)' }
    else { Write-Step ('Luma/ReShade dxgi.dll: ' + (Disable-Luma)) }
    [void](Save-RuntimeFiles ($runStamp + '-stale'))
    if ($NoMod) {
        if (Read-DeployManifest) { [void](Invoke-Undeploy) }
        Write-Step 'Running without the mod (-NoMod)'
    } else {
        [void](Invoke-Deploy -buildDir $BuildDir -iniPath $Ini)
    }

    $envVars = @{}
    # 'powershell -File' passes an array argument as one string, so ';' also separates entries.
    foreach ($kv in @($GameEnv | ForEach-Object { $_ -split ';' } | Where-Object { $_ })) {
        $i = $kv.IndexOf('=')
        if ($i -lt 1) { throw "Bad -GameEnv entry '$kv' (expected NAME=value)" }
        $envVars[$kv.Substring(0, $i)] = $kv.Substring($i + 1)
    }

    if (-not (Start-SteamIfNeeded)) { throw 'Steam did not start' }
    $gameArgs = Get-GameArguments -width $Width -height $Height -fullscreen:$Fullscreen -extra $ExtraArgs
    $t0 = Get-Date
    if ($Via -eq 'steam') {
        if ($envVars.Count -gt 0) { Write-Step 'WARNING: -GameEnv has no effect with -Via steam' }
        Start-GameViaSteam $gameArgs
    } else {
        [void](Start-GameDirect $gameArgs $envVars)
    }

    $hwnd = Wait-Until { $h = Get-GameWindow; if ($h -ne [IntPtr]::Zero) { $h } } $StartTimeout 500 'the game window'
    if (-not $hwnd) { throw 'The game window did not appear' }
    $gp = @(Get-GameProcesses)[0]
    Write-Step ("Game window after {0:N1} s (pid {1})" -f ((Get-Date) - $t0).TotalSeconds, $gp.Id)

    if (-not $NoMod) {
        $logPath = Get-GameLogPath
        $loaded = Wait-Until { if (Test-LogContains $logPath 'ff7vr: initialised') { 'yes' } elseif (@(Get-GameProcesses).Count -eq 0) { 'exited' } } 60 500 'ff7vr.log to show the mod loaded'
        if ($loaded -ne 'yes') { throw "The mod did not report in $logPath ($(if ($loaded) { 'game exited' } else { 'timeout' }))" }
        $first = @((Read-SharedText $logPath) -split "`r?`n" | Where-Object { $_ -match ' loaded \(commit ' })
        if ($first.Count -gt 0) { Write-Step ("Mod loaded: " + $first[0].Substring(24)) }
    }

    if ($Until -eq 'title') {
        if (-not (Wait-ScreenState @('title') $UntilTimeout)) { $failures += 'title screen not reached' }
        else { Write-Step ("Title screen after {0:N1} s" -f ((Get-Date) - $t0).TotalSeconds) }
    } elseif ($Until -eq 'gameplay') {
        $shotDir = Join-Path $script:CapturesDir ("runs\$runStamp-steps")
        if (Invoke-ReachGameplay -timeoutSeconds $UntilTimeout -shotDir $shotDir) {
            Write-Step ("Gameplay reached after {0:N1} s (step captures in {1})" -f ((Get-Date) - $t0).TotalSeconds, $shotDir)
        } else { $failures += 'gameplay not reached' }
    }

    if ($WaitLog -and -not $NoMod) {
        if (-not (Wait-Until { Test-LogContains (Get-GameLogPath) $WaitLog } $UntilTimeout 500 "log pattern '$WaitLog'")) {
            $failures += "log pattern '$WaitLog' not seen"
        }
    }
    if ($WaitSeconds -gt 0) {
        Write-Step "Waiting $WaitSeconds s"
        $end = (Get-Date).AddSeconds($WaitSeconds)
        while ((Get-Date) -lt $end -and @(Get-GameProcesses).Count -gt 0) { Start-Sleep -Milliseconds 500 }
    }

    if (($Screenshot -or $ScreenshotPath) -and @(Get-GameProcesses).Count -eq 0) {
        Write-Step 'No screenshot: the game is not running'
    } elseif ($Screenshot -or $ScreenshotPath) {
        $path = $ScreenshotPath
        if (-not $path) { $path = New-CapturePath $Until }
        elseif (-not [System.IO.Path]::IsPathRooted($path)) { $path = Join-Path (Get-Location) $path }
        $shot = Save-GameScreenshot -path $path
        $bmp = New-Object System.Drawing.Bitmap $shot.Path
        try { $state = (Get-FrameState $bmp).State } finally { $bmp.Dispose() }
        Write-Step ("Screenshot {0} ({1}x{2}, mean {3}, state {4})" -f $shot.Path, $shot.Width, $shot.Height, $shot.Mean, $state)
        if ($shot.Blank) { $failures += 'screenshot is blank' }
    }

    if (@(Get-GameProcesses).Count -eq 0) { $failures += 'the game exited during the run' }
    if (-not $NoMod) {
        $crashes = @((Read-SharedText (Get-GameLogPath)) -split "`r?`n" | Where-Object { $_ -match 'FATAL CRASH' })
        if ($crashes.Count -gt 0) { $failures += "crash reported in ff7vr.log: $($crashes[0])" }
    }
}
catch {
    $failures += "$_"
}
finally {
    if ($KeepRunning -and @(Get-GameProcesses).Count -gt 0 -and $failures.Count -eq 0) {
        Write-Step "Game left running (lock held by '$owner'). Run tools\dev\stop.ps1 when done."
    } elseif ($startedLock) {
        Write-Step 'Cleaning up'
        if (-not (Stop-GameRun -owner $owner)) { $failures += 'cleanup incomplete (see messages above)' }
    }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Step "FAILED: $_" }
    exit 1
}
Write-Step 'OK'
exit 0
