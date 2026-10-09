# Helpers for the picture-quality steps files (picture-*.ps1): console variable values,
# A/B frame time windows and still/turning captures of the same view.
# Dot-sourced by the steps files themselves (they run inside vr-perf-session.ps1).

# The game's value of a console variable (float text), or $null if the game has no such variable.
function Get-CvarValue([string]$name) {
    $r = @(Send-DevCommand @("cvar get $name"))[0]
    if ($r -match 'float=([-\d.e+]+)') { return $Matches[1] }
    return $null
}

# Writes the values of the given variables to <out>\cvars.txt (one "name = int / float setby" line each).
function Save-CvarSurvey([string[]]$names, [string]$path) {
    $lines = foreach ($n in $names) { "{0,-44} {1}" -f $n, @(Send-DevCommand @("cvar get $n"))[0] }
    $lines | Set-Content -LiteralPath $path -Encoding UTF8
    $lines | ForEach-Object { Write-Host "  $_" }
}

# Sets several variables ("name value" pairs, ';'-separated) and waits for temporal AA and
# exposure to settle.
function Set-Cvars([string]$pairs, [int]$settleMs = 2500) {
    foreach ($p in @($pairs -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) { P "cvar set $p" | Out-Null }
    Start-Sleep -Milliseconds $settleMs
}

# A still capture of both eyes; with -Turning also one while the emulated head turns (the
# Null backend's yaw sweep, xr-sim motion yaw), after which the head is still again, so the
# next still capture shows the same view. The game camera is not touched (mouse turns of the
# third-person camera do not come back to the same view).
function Capture-View([string]$name, [switch]$Turning) {
    P ("capture $out\$name") | Out-Null
    if ($Turning) {
        P 'xr-sim motion yaw' | Out-Null
        Start-Sleep -Milliseconds 1200
        P ("capture $out\$name-turn") | Out-Null
        P 'xr-sim motion static' | Out-Null
        Start-Sleep -Milliseconds 1500
    }
}

# Alternating windows: $rounds x ($sec s with the base values, $sec s with the candidate).
# Each value is a ';'-separated "name value" list (applied with Set-Cvars).
function AB-Cvars([string]$label, [string]$base, [string]$cand, [int]$rounds = 2, [int]$sec = 15) {
    foreach ($round in 1..$rounds) {
        Set-Cvars $base 1500
        Measure-Perf "$label-base-$round" $sec | Out-Null
        Set-Cvars $cand 1500
        Measure-Perf "$label-cand-$round" $sec | Out-Null
    }
    Set-Cvars $base 1000
}

# The same with dev pipe commands instead of console variables (';'-separated).
function AB-Commands([string]$label, [string]$base, [string]$cand, [int]$rounds = 2, [int]$sec = 15) {
    foreach ($round in 1..$rounds) {
        P $base | Out-Null; Start-Sleep -Milliseconds 1500
        Measure-Perf "$label-base-$round" $sec | Out-Null
        P $cand | Out-Null; Start-Sleep -Milliseconds 1500
        Measure-Perf "$label-cand-$round" $sec | Out-Null
    }
    P $base | Out-Null
}
