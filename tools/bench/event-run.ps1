<#
.SYNOPSIS
  One short game run that times dev pipe commands after gameplay and keeps the log: for
  measuring what single events (a DLSS dropout, stereo off/on, an XR session restart) create
  on the graphics device (docs\benchmarking.md, "Allocations per event").

.DESCRIPTION
  Starts the game to gameplay with the build in -BuildDir (the Null backend unless -Set says
  otherwise), sends each item of -Seq at its time, writes every command and reply to run.txt,
  stops the game -EndSeconds after gameplay and copies ff7vr.log next to it.

  -Seq is a list of "<seconds after gameplay>:<dev pipe command>" items, separated by '|'.
  Several commands at one time: separate them with ';' inside the item.

  Output: captures\events\<stamp>-<tag>\ (run.txt, ff7vr.log).

.EXAMPLE
  $env:FF7VR_DEV_NAME = 'me'
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\event-run.ps1 -Tag dropout `
      -BuildDir build\release-dlss -Set "xr.backend=null;log.d3d11_census=1" `
      -Seq "40:census mark start|60:census mark idle;dlss off|80:census mark off;dlss on|100:census mark on" -EndSeconds 110
#>
param(
    [Parameter(Mandatory)] [string]$Tag,
    [string]$BuildDir = '',
    [string]$Ini = '',
    [string]$Set = '',
    [string]$Seq = '',
    [int]$EndSeconds = 300,
    # The harness's screen test takes some bright areas for a dialog and stops before gameplay.
    # With -ManualContinue the game starts with stereo off, the run waits for the title screen,
    # presses Enter three times (title, Continue, the resume prompt), waits -LoadSeconds and
    # switches stereo on through the pipe; the timed items count from there.
    [switch]$ManualContinue,
    [int]$LoadSeconds = 25,
    [int]$LockWaitSeconds = 2400
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\dev\common.ps1"
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$out = Join-Path $script:CapturesDir "events\$stamp-$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
$runLog = Join-Path $out 'run.txt'
function Note([string]$m) {
    $l = "[{0}] {1}" -f (Get-Date).ToString('HH:mm:ss'), $m
    Add-Content -LiteralPath $runLog -Value $l -Encoding utf8
    Write-Host $l
}
$items = @()
foreach ($raw in @($Seq -split '\|' | Where-Object { $_.Trim() })) {
    if ($raw -notmatch '^\s*(\d+)\s*:(.+)$') { Write-Host "bad -Seq item '$raw' (expected <seconds>:<command>)"; exit 2 }
    $items += [pscustomobject]@{ At = [int]$Matches[1]; Cmds = @($Matches[2] -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
}
$items = @($items | Sort-Object At)
Note "tag $Tag, build '$BuildDir', set '$Set', $($items.Count) timed items, end at $EndSeconds s"

$until = if ($ManualContinue) { 'title' } else { 'gameplay' }
if ($ManualContinue) { $Set = (@($Set, 'stereo.start_in_stereo=0') | Where-Object { $_ }) -join ';' }
$launch = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot '..\dev\launch.ps1'), '-Until', $until, '-KeepRunning',
            '-LockWaitSeconds', $LockWaitSeconds)
if ($BuildDir) { $launch += @('-BuildDir', $BuildDir) }
if ($Ini) { $launch += @('-Ini', $Ini) }
if ($Set) { $launch += @('-Set', $Set) }
& powershell @launch *>&1 | ForEach-Object { Add-Content -LiteralPath (Join-Path $out 'launch.txt') -Value "$_" -Encoding utf8 }
if ($LASTEXITCODE -ne 0) {
    Note "launch failed (exit $LASTEXITCODE), see launch.txt; stopping"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot '..\dev\stop.ps1') *>&1 | Out-Null
    exit 1
}
if ($ManualContinue) {
    for ($i = 0; $i -lt 3; $i++) { Send-GameKey 'enter' 120; Start-Sleep -Milliseconds 1800 }
    Start-Sleep -Seconds $LoadSeconds
    Note ("stereo on -> {0}" -f (@(Send-DevCommand @('stereo on'))[0]))
}
$t0 = Get-Date
Note 'gameplay reached'
try {
    foreach ($it in $items) {
        while (((Get-Date) - $t0).TotalSeconds -lt $it.At) { Start-Sleep -Milliseconds 250 }
        if (@(Get-GameProcesses).Count -eq 0) { Note 'the game is gone'; break }
        $replies = @(Send-DevCommand $it.Cmds)
        for ($i = 0; $i -lt $it.Cmds.Count; $i++) { Note ("{0,4} s  {1} -> {2}" -f $it.At, $it.Cmds[$i], $(if ($i -lt $replies.Count) { $replies[$i] } else { '(no reply)' })) }
    }
    while (((Get-Date) - $t0).TotalSeconds -lt $EndSeconds -and @(Get-GameProcesses).Count -gt 0) { Start-Sleep -Milliseconds 500 }
} finally {
    $log = Get-GameLogPath
    if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log (Join-Path $out 'ff7vr.log') -Force }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot '..\dev\stop.ps1') *>&1 | ForEach-Object { Add-Content -LiteralPath (Join-Path $out 'stop.txt') -Value "$_" -Encoding utf8 }
    Note "stopped (exit $LASTEXITCODE); output in $out"
}
