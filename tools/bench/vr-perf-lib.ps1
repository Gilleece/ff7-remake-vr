# Helpers for tools\bench\vr-perf-session.ps1 and its steps files. Dot-source after
# tools\dev\common.ps1 and tools\bench\bench-common.ps1.
$script:PerfResults = New-Object System.Collections.ArrayList

# Sends ';'-separated commands to the mod's dev pipe and prints the replies.
function P([string]$cmds) {
    $lines = @($cmds -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $r = @(Send-DevCommand $lines)
    for ($i = 0; $i -lt $r.Count; $i++) { Write-Host ("  > {0} -> {1}" -f $lines[$i], $r[$i]) }
    return $r
}

function Get-LogAfterMark([string]$mark) {
    $text = Read-SharedText (Get-GameLogPath)
    $idx = $text.LastIndexOf("MARK $mark")
    if ($idx -lt 0) { return @() }
    return @($text.Substring($idx) -split "`r?`n")
}

# One measurement: a frame time window of $sec seconds (`stereo frametime`) and the scene /
# after-scene GPU times (`fov timing`) over the same period.
function Measure-Perf([string]$label, [int]$sec = 5) {
    $null = Send-DevCommand @('fov timing')   # drop the samples collected so far
    $null = Send-DevCommand @("mark perf-$label", "stereo frametime $sec")
    Start-Sleep -Milliseconds ([int](1000 * $sec + 900))
    $fov = @(Send-DevCommand @('fov timing'))[0]
    $pattern = 'frame time: (\d+) frames, avg ([\d.]+) ms, median ([\d.]+), p95 ([\d.]+), max ([\d.]+) \(([^,]+)'
    $ft = Get-LogAfterMark "perf-$label" | Where-Object { $_ -match $pattern } | Select-Object -First 1
    $o = [ordered]@{ label = $label; frames = 0; avg = 0; p50 = 0; p95 = 0; max = 0; mode = ''; sceneP50 = 0; sceneAvg = 0; afterP50 = 0; afterAvg = 0 }
    if ($ft -and ($ft -match $pattern)) {
        $o.frames = [int]$Matches[1]; $o.avg = [double]$Matches[2]; $o.p50 = [double]$Matches[3]; $o.p95 = [double]$Matches[4]
        $o.max = [double]$Matches[5]; $o.mode = $Matches[6]
    }
    if ($fov -match 'gpu scene \(foveation window\): avg ([\d.]+) p50 ([\d.]+)') { $o.sceneAvg = [double]$Matches[1]; $o.sceneP50 = [double]$Matches[2] }
    if ($fov -match 'gpu after the scene until Present: avg ([\d.]+) p50 ([\d.]+)') { $o.afterAvg = [double]$Matches[1]; $o.afterP50 = [double]$Matches[2] }
    $obj = [pscustomobject]$o
    [void]$script:PerfResults.Add($obj)
    Write-Host ("RESULT {0,-28} frame avg {1,6:N2} p50 {2,6:N2} p95 {3,6:N2} | scene p50 {4,6:N2} | after p50 {5,6:N2} | n {6} {7}" -f `
            $label, $o.avg, $o.p50, $o.p95, $o.sceneP50, $o.afterP50, $o.frames, $o.mode)
    return $obj
}

function Save-PerfResults([string]$path) {
    $script:PerfResults | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $path -Encoding UTF8
}

# Turns the camera with relative mouse moves (game window in the foreground). Stop with .Stop().
function Start-Pan([int]$step = 6) {
    $p = New-Object FF7VR.MousePan
    Set-GameForeground (Get-GameWindow) | Out-Null
    $p.Start($step, 0, 10)
    return $p
}

# Walks forward with W for $ms milliseconds.
function Send-Walk([int]$ms) {
    $repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$repoRoot\tools\dev\send-input.ps1" -Keys w -HoldMs $ms | Out-Null
}
