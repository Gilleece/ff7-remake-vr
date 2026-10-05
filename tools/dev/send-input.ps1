<#
.SYNOPSIS
  Send keyboard input to the game, or commands to the mod's dev pipe.

.DESCRIPTION
  -Keys sends a comma-separated sequence through SendInput (scan codes). The
  game window is brought to the foreground first; keyboard input only reaches
  the game while it is the foreground window. Tokens:
    enter, esc, space, tab, up, down, left, right, a-z, 0-9, f1-f12, ...
    <key>*N       press N times (e.g. down*3)
    wait:<ms>     pause
  -DelayMs is the pause between presses (default 400; menus ignore presses
  that come too fast).

  -Pipe sends raw lines (separated by ';') to \\.\pipe\ff7vr-dev, served by the mod when
  ff7vr.ini has [dev] pipe=1 (see src\loader\dev_input.h): 'ping',
  'mark <text>' (writes a marker into ff7vr.log), and virtual XInput pad
  commands ('tap A', 'stick L 0 1 500', ...). Note: in the current game build
  the virtual pad has no effect because the game does not poll XInput when no
  physical controller is present (ping reports xinput_calls=0); use -Keys.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\send-input.ps1 -Keys enter
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\send-input.ps1 -Keys "down*2,wait:500,enter"
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\send-input.ps1 -Pipe "ping;mark before-test"
#>
param(
    [string]$Keys = '',
    [string[]]$Pipe = @(),
    [int]$DelayMs = 400,
    [int]$HoldMs = 120
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

if (-not $Keys -and $Pipe.Count -eq 0) { Write-Host 'Nothing to send: give -Keys and/or -Pipe'; exit 1 }

if ($Keys) {
    foreach ($tok in ($Keys -split ',')) {
        $t = $tok.Trim()
        if (-not $t) { continue }
        if ($t -match '^wait:(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
        $count = 1
        if ($t -match '^(.+)\*(\d+)$') { $t = $Matches[1]; $count = [int]$Matches[2] }
        for ($i = 0; $i -lt $count; $i++) {
            Send-GameKey $t $HoldMs
            Write-Step "key $t"
            Start-Sleep -Milliseconds $DelayMs
        }
    }
}

# 'powershell -File' passes an array argument as one string, so ';' also separates commands.
$Pipe = @($Pipe | ForEach-Object { $_ -split ';' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($Pipe.Count -gt 0) {
    $replies = @(Send-DevCommand $Pipe)
    for ($i = 0; $i -lt $replies.Count; $i++) { Write-Output ("{0} -> {1}" -f $Pipe[$i], $replies[$i]) }
    if (@($replies | Where-Object { $_ -notlike 'ok*' }).Count -gt 0) { exit 1 }
}
exit 0
