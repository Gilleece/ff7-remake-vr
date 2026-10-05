<#
.SYNOPSIS
  Run one benchmark configuration end to end and write a result JSON.

.DESCRIPTION
  1. Takes the game lock (and for VR configurations the SteamVR lock).
  2. VR configurations: switches SteamVR to its null driver with the
     configuration's render size and refresh rate, starts SteamVR.
     UEVR configurations: backs up UEVR's profile folder for this game (once,
     verified) and writes the run's config.txt (profile + the configuration's
     UevrConfig keys + menu closed).
  3. Starts the game through tools\dev\launch.ps1 with the mod DLL loaded for
     measurement only (bench.ini: frame timer), windowed at the
     configuration's size, and loads the latest save (-Until gameplay).
  4. UEVR configurations: injects UEVR (nullifier, openxr_loader.dll,
     UEVRBackend.dll, like UEVRInjector.exe) and waits until UEVR's log shows
     the framework, the OpenXR swapchains and the stereo device.
  5. Warm-up, then records frame times at Present for -RecordSeconds while
     sampling the GPU with nvidia-smi and the game's CPU time.
  6. Stops everything and restores: game stopped, mod undeployed, Luma's
     dxgi.dll back, UEVR profile restored and verified, SteamVR stopped and its
     settings restored, locks released. This runs also when a step fails.
  7. Writes result.json (and frames.csv, gpu.csv, logs, a screenshot) to
     captures\bench\<time>-<config>\.

  Exit code: 0 ok, 1 a step failed, 3 a lock could not be taken.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\bench-run.ps1 -Config flat-720p
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\bench-run.ps1 -Config uevr-2496 -RecordSeconds 60
#>
param(
    [Parameter(Mandatory = $true)][string]$Config,
    [string]$ConfigFile = '',
    [int]$WarmupSeconds = 20,
    [int]$RecordSeconds = 45,
    [ValidateSet('idle')]
    [string]$Scene = 'idle',
    [string]$OutDir = '',
    [string]$BuildDir = '',
    [string]$UevrDir = '',
    [string]$Label = '',
    [int]$LockWaitSeconds = 1800
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\bench-common.ps1"

if (-not $ConfigFile) { $ConfigFile = Join-Path $PSScriptRoot 'configs.psd1' }
$all = (Import-PowerShellDataFile -LiteralPath $ConfigFile).Configs
if (-not $all.ContainsKey($Config)) { Write-Step "Unknown configuration '$Config'. Known: $(($all.Keys | Sort-Object) -join ', ')"; exit 1 }
$cfg = $all[$Config]
# Optional keys of the configuration ($null when absent; strict mode forbids reading a missing key directly).
function Get-Cfg([string]$key) { if ($cfg.ContainsKey($key)) { return $cfg[$key] } return $null }
$kind = $cfg.Kind
$isVr = ($kind -eq 'uevr' -or $kind -eq 'mod')
$owner = Get-DefaultOwner
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
if (-not $OutDir) { $OutDir = Join-Path $script:BenchResultsDir "$stamp-$Config" }
Ensure-Dir $OutDir
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$devDir = Join-Path $PSScriptRoot '..\dev'
$benchIni = Join-Path $PSScriptRoot 'bench.ini'
$failures = @()
$notes = @()
$haveGameLock = $false
$haveSvLock = $false
$svEnabled = $false
$uevrRun = $false
$uevr = $null
$result = [ordered]@{
    config = $Config; description = (Get-Cfg Description); kind = $kind; label = $Label; scene = $Scene
    started = (Get-Date).ToString('o'); warmupSeconds = $WarmupSeconds; recordSeconds = $RecordSeconds
    window = [ordered]@{ width = $cfg.Width; height = $cfg.Height }
}
Write-Step "Benchmark '$Config' ($((Get-Cfg Description))), results in $OutDir"

function Invoke-DevScript([string]$name, [string[]]$arguments) {
    $p = Join-Path $devDir $name
    & powershell -NoProfile -ExecutionPolicy Bypass -File $p @arguments | ForEach-Object { Write-Host $_ }
    return $LASTEXITCODE
}

function Send-Bench([string]$cmd) {
    $r = @(Send-DevCommand @($cmd) 5000)
    if ($r.Count -eq 0) { return '' }
    return [string]$r[0]
}

function ConvertFrom-KvReply([string]$reply) {
    $h = [ordered]@{}
    foreach ($t in ($reply -split '\s+')) { if ($t -match '^([^=]+)=(.*)$') { $h[$Matches[1]] = $Matches[2] } }
    return $h
}

$cvarNames = @('r.ScreenPercentage', 'r.DynamicRes.OperationMode', 'r.DynamicRes.MinScreenPercentage',
               'r.DynamicRes.MaxScreenPercentage', 'r.DynamicRes.FrameTimeBudget', 'r.HZBOcclusion', 'r.TemporalAASamples', 't.MaxFPS', 'r.VSync',
               'r.AllowOcclusionQueries', 'r.InstancedStereo', 'r.DefaultFeature.AntiAliasing', 'r.PostProcessAAQuality',
               'sg.ResolutionQuality', 'sg.ShadowQuality')

try {
    # ---------------------------------------------------------------- locks
    if (-not (Lock-Game -owner $owner -waitSeconds $LockWaitSeconds)) { Write-Step 'Could not get the game lock'; exit 3 }
    $haveGameLock = $true
    if (@(Get-GameProcesses).Count -gt 0) { throw 'ff7remake_ is already running' }
    if ($isVr) {
        if (-not (Lock-SteamVr -owner $owner -waitSeconds $LockWaitSeconds)) { throw 'Could not get the SteamVR lock' }
        $haveSvLock = $true
    }

    # ---------------------------------------------------------------- SteamVR null driver
    $gameEnv = @()
    if ($isVr) {
        $sv = (Get-Cfg SteamVR)
        $svArgs = @('-StopSteamVr')
        if ($sv.RenderWidth) { $svArgs += @('-RenderWidth', [string]$sv.RenderWidth) }
        if ($sv.RenderHeight) { $svArgs += @('-RenderHeight', [string]$sv.RenderHeight) }
        if ($sv.RefreshHz) { $svArgs += @('-RefreshHz', [string]$sv.RefreshHz) }
        $svEnabled = $true
        if ((Invoke-DevScript 'steamvr-null-enable.ps1' $svArgs) -ne 0) { throw 'steamvr-null-enable.ps1 failed' }
        if ((Invoke-DevScript 'steamvr-start.ps1' @('-Owner', $owner)) -ne 0) { throw 'steamvr-start.ps1 failed' }
        $runtimeJson = Get-SteamVrRuntimeJson
        $gameEnv += "XR_RUNTIME_JSON=$runtimeJson"
        $layerEnv = Get-ImplicitLayerDisableEnv
        foreach ($k in $layerEnv.Keys) { $gameEnv += "$k=$($layerEnv[$k])" }
        $result.steamvr = [ordered]@{ runtimeJson = $runtimeJson; renderWidth = $sv.RenderWidth; renderHeight = $sv.RenderHeight;
                                      refreshHz = $sv.RefreshHz; implicitLayersDisabled = @($layerEnv.Keys) }
    }

    # ---------------------------------------------------------------- UEVR profile for this run
    if ($kind -eq 'uevr') {
        $uevr = Find-UevrBuild $UevrDir
        Write-Step "UEVR build: $($uevr.Dir) (commit $($uevr.Commit), $($uevr.Reason))"
        $backup = Enter-UevrProfileRun
        $uevrRun = $true
        $vals = @{ FrameworkConfig_RememberMenuState = 'true'; FrameworkConfig_MenuOpen = 'false' }
        if ((Get-Cfg UevrConfig)) { foreach ($k in (Get-Cfg UevrConfig).Keys) { $vals[$k] = [string](Get-Cfg UevrConfig)[$k] } }
        Set-UevrConfigValues $vals
        # The backend appends to log.txt only within one run; remove the old one so the run's log is clean.
        Remove-Item -LiteralPath (Join-Path (Get-UevrProfileDir) 'log.txt') -Force -ErrorAction SilentlyContinue
        $result.uevr = [ordered]@{ dir = $uevr.Dir; commit = $uevr.Commit; profileBackup = $backup; overrides = $vals;
                                   settings = (Get-UevrConfigValues) }
    }

    # ---------------------------------------------------------------- start the game into the save
    $set = @()
    if ((Get-Cfg Set)) { $set += @((Get-Cfg Set)) }
    $launchArgs = @('-Until', 'gameplay', '-KeepRunning', '-Ini', $benchIni, '-Width', [string]$cfg.Width, '-Height', [string]$cfg.Height,
                    '-LockWaitSeconds', '0')
    if ($BuildDir) { $launchArgs += @('-BuildDir', $BuildDir) }
    if ($set.Count -gt 0) { $launchArgs += @('-Set', ($set -join ';')) }
    if ($gameEnv.Count -gt 0) { $launchArgs += @('-GameEnv', ($gameEnv -join ';')) }
    $t0 = Get-Date
    $rc = Invoke-DevScript 'launch.ps1' $launchArgs
    if ($rc -ne 0) { throw "launch.ps1 failed (exit $rc)" }
    $game = @(Get-GameProcesses)[0]
    $result.gameplayAfterSeconds = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    $result.gamePid = $game.Id

    # ---------------------------------------------------------------- UEVR injection
    if ($kind -eq 'uevr') {
        [void](Set-GameForeground (Get-GameWindow))
        Invoke-UevrInject -gamePid $game.Id -uevrDir $uevr.Dir
        $uevrLog = Join-Path (Get-UevrProfileDir) 'log.txt'
        $ok = Wait-Until {
            $f = Get-UevrLogFacts $uevrLog
            if ($f.frameworkInitialized -and $f.swapchainsCreated -and (Test-LogContains $uevrLog 'Stereo rendering device setup successfully|Found active stereo device')) { $f }
            elseif (@(Get-GameProcesses).Count -eq 0) { 'exited' }
        } 120 1000 'UEVR to initialise (framework, OpenXR swapchains, stereo device)'
        if ($ok -eq 'exited') { throw 'The game exited after UEVR was injected' }
        if (-not $ok) { throw 'UEVR did not report a working stereo setup within 120 s (see uevr-log.txt)' }
        Write-Step ("UEVR running: OpenXR system '{0}', double-wide swapchain {1}x{2}" -f $ok.openxrSystem, $ok.swapchainWidth, $ok.swapchainHeight)
    }

    # ---------------------------------------------------------------- console variables for this run
    $st = Send-Bench 'bench status'
    if ($st -notmatch 'hooked=1') { throw "Frame timer not running: '$st'" }
    if ((Get-Cfg Cvars)) {
        $result.cvarsSet = [ordered]@{}
        foreach ($k in (Get-Cfg Cvars).Keys) {
            $rep = Send-Bench "bench setcvar $k $((Get-Cfg Cvars)[$k])"
            $result.cvarsSet[$k] = $rep
            Write-Step "cvar $k = $((Get-Cfg Cvars)[$k]): $rep"
            if ($rep -notlike 'ok*') { throw "Could not set $k ($rep)" }
        }
    }

    # ---------------------------------------------------------------- warm-up and record
    Write-Step "Warm-up $WarmupSeconds s"
    [void](Set-GameForeground (Get-GameWindow))
    $end = (Get-Date).AddSeconds($WarmupSeconds)
    while ((Get-Date) -lt $end) {
        if (@(Get-GameProcesses).Count -eq 0) { throw 'The game exited during warm-up' }
        Start-Sleep -Milliseconds 500
    }
    $cvBefore = Send-Bench ('bench cvars ' + ($cvarNames -join ' '))
    $hwnd = Get-GameWindow
    [void](Set-GameForeground $hwnd)
    $gpuCsv = Join-Path $OutDir 'gpu.csv'
    $frameCsv = Join-Path $OutDir 'frames.csv'
    $proc = Get-Process -Id $game.Id
    $cpu0 = $proc.TotalProcessorTime.TotalMilliseconds
    $sampler = Start-GpuSampler 250
    $r = Send-Bench 'bench start'
    if ($r -notlike 'ok*') { throw "bench start failed: $r" }
    $w0 = [Diagnostics.Stopwatch]::StartNew()
    Write-Step "Recording $RecordSeconds s (scene: $Scene)"
    $fgSamples = 0; $fgHits = 0
    while ($w0.Elapsed.TotalSeconds -lt $RecordSeconds) {
        if (@(Get-GameProcesses).Count -eq 0) { throw 'The game exited during recording' }
        $fgSamples++
        if ([FF7VR.Native]::GetForegroundWindow() -eq $hwnd) { $fgHits++ }
        Start-Sleep -Milliseconds 1000
    }
    $r = Send-Bench "bench stop $frameCsv"
    $wall = $w0.Elapsed.TotalMilliseconds
    $proc.Refresh()
    $cpu1 = $proc.TotalProcessorTime.TotalMilliseconds
    $gpu = @(Stop-GpuSampler $sampler $gpuCsv)
    if ($r -notlike 'ok*') { throw "bench stop failed: $r" }
    $status = ConvertFrom-KvReply (Send-Bench 'bench status')
    $cvars = ConvertFrom-KvReply $cvBefore
    try {
        $shot = Save-GameScreenshot -path (Join-Path $OutDir 'game.png')
        $result.screenshot = [ordered]@{ path = $shot.Path; mean = $shot.Mean; blank = $shot.Blank }
    } catch { $notes += "screenshot failed: $_" }

    # ---------------------------------------------------------------- numbers
    $fs = Get-FrameStats $frameCsv
    if (-not $fs) { throw 'No frames recorded' }
    $nCpu = [Environment]::ProcessorCount
    $result.frames = $fs
    $result.gpu = [ordered]@{ samples = $gpu.Count; info = (Get-GpuInfo) }
    if ($gpu.Count -gt 0) {
        $result.gpu.utilPct = [math]::Round(($gpu | Measure-Object Util -Average).Average, 1)
        $result.gpu.utilPctMin = ($gpu | Measure-Object Util -Minimum).Minimum
        $result.gpu.memUsedMiB = [math]::Round(($gpu | Measure-Object MemUsedMiB -Average).Average, 0)
        $result.gpu.memUsedMiBMax = [math]::Round(($gpu | Measure-Object MemUsedMiB -Maximum).Maximum, 0)
        $result.gpu.powerW = [math]::Round(($gpu | Measure-Object PowerW -Average).Average, 1)
        $result.gpu.clockMHz = [math]::Round(($gpu | Measure-Object ClockMHz -Average).Average, 0)
        $result.gpu.tempC = [math]::Round(($gpu | Measure-Object TempC -Average).Average, 0)
    }
    $result.cpu = [ordered]@{
        processCpuSeconds = [math]::Round(($cpu1 - $cpu0) / 1000.0, 2)
        coresBusy = [math]::Round(($cpu1 - $cpu0) / $wall, 2)          # 1.0 = one logical core fully busy
        machinePct = [math]::Round(100.0 * ($cpu1 - $cpu0) / $wall / $nCpu, 1)
        logicalCores = $nCpu
    }
    $result.foregroundPct = [math]::Round(100.0 * $fgHits / [math]::Max(1, $fgSamples), 0)
    if ($result.foregroundPct -lt 100) { $notes += "game window was in the foreground for $($result.foregroundPct)% of the recording" }
    $result.backBuffer = "$($status['backbuffer'])"
    $result.presentSyncInterval = $status['sync']
    $result.cvars = $cvars

    # Resolution actually rendered.
    if ($kind -eq 'uevr') {
        Copy-Item -LiteralPath (Join-Path (Get-UevrProfileDir) 'log.txt') -Destination (Join-Path $OutDir 'uevr-log.txt') -Force
        $facts = Get-UevrLogFacts (Join-Path $OutDir 'uevr-log.txt')
        $result.uevr.log = [ordered]@{ commit = $facts.commit; version = "$($facts.tag)+$($facts.commitsPastTag)"; frameworkInitialized = $facts.frameworkInitialized;
                                       d3d = $facts.d3d; openxrSystem = $facts.openxrSystem; requestedRuntime = $facts.requestedRuntime;
                                       swapchain = "$($facts.swapchainWidth)x$($facts.swapchainHeight)"; gengineFound = $facts.gengineFound;
                                       tickFound = $facts.tickFound; plugin = $facts.pluginLines; stereo = $facts.vrLines; errors = $facts.errors }
        $eyeW = [int]($facts.swapchainWidth / 2); $eyeH = $facts.swapchainHeight
        $result.eyeResolution = "${eyeW}x${eyeH}"
        $result.renderedPixels = 2 * $eyeW * $eyeH
        $result.resolutionSource = 'UEVR log: double-wide eye swapchain'
    } else {
        $sp = 100.0
        if ($cvars.Contains('r.ScreenPercentage')) { [void][double]::TryParse($cvars['r.ScreenPercentage'], [ref]$sp) }
        $bb = "$($status['backbuffer'])" -split 'x'
        $result.eyeResolution = $null
        $result.renderedPixels = [int]([double]$bb[0] * [double]$bb[1] * ($sp / 100.0) * ($sp / 100.0))
        $result.resolutionSource = "back buffer $($status['backbuffer']) x r.ScreenPercentage $sp"
    }
}
catch {
    $failures += "$_"
    Write-Step "ERROR: $_"
}
finally {
    # ---------------------------------------------------------------- cleanup, always
    Write-Step 'Cleaning up'
    try {
        if ($uevrRun -and (Test-Path -LiteralPath (Join-Path (Get-UevrProfileDir) 'log.txt')) -and -not (Test-Path -LiteralPath (Join-Path $OutDir 'uevr-log.txt'))) {
            Copy-Item -LiteralPath (Join-Path (Get-UevrProfileDir) 'log.txt') -Destination (Join-Path $OutDir 'uevr-log.txt') -Force
        }
    } catch { }
    $logsBefore = @(Get-ChildItem -LiteralPath (Join-Path $script:CapturesDir 'runs') -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    if ($haveGameLock) {
        if (-not (Stop-GameRun -owner $owner)) { $failures += 'game cleanup incomplete' }
        $haveGameLock = $false
    }
    $newLogs = @(Get-ChildItem -LiteralPath (Join-Path $script:CapturesDir 'runs') -Directory -ErrorAction SilentlyContinue |
                 Where-Object { $logsBefore -notcontains $_.Name } | ForEach-Object { $_.FullName })
    foreach ($d in $newLogs) {
        $l = Join-Path $d 'ff7vr.log'
        if (Test-Path -LiteralPath $l) { Copy-Item -LiteralPath $l -Destination (Join-Path $OutDir 'ff7vr.log') -Force }
    }
    if ($uevrRun) {
        $p = @(Exit-UevrProfileRun)
        $result.uevrProfileRestored = ($p.Count -eq 0)
        if ($p.Count -gt 0) { $failures += "UEVR profile not restored: $($p -join ', ')" }
    }
    if ($isVr) {
        if ((Invoke-DevScript 'steamvr-stop.ps1' @('-Owner', $owner)) -ne 0) { $failures += 'steamvr-stop.ps1 failed' }
        if ($svEnabled -and (Invoke-DevScript 'steamvr-null-restore.ps1' @()) -ne 0) { $failures += 'steamvr-null-restore.ps1 failed' }
        if ($haveSvLock) { [void](Unlock-SteamVr -owner $owner) }
    }
    $result.finished = (Get-Date).ToString('o')
    $result.failures = $failures
    $result.notes = $notes
    $result.machine = [ordered]@{ computer = 'local'; os = [Environment]::OSVersion.VersionString; logicalCores = [Environment]::ProcessorCount;
                                  gameExe = $null }
    try {
        $vi = (Get-Item -LiteralPath (Get-GameExe)).VersionInfo
        $result.machine.gameExe = '{0}.{1}.{2}.{3}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart
    } catch { }
    $result.commandLine = "bench-run.ps1 -Config $Config -WarmupSeconds $WarmupSeconds -RecordSeconds $RecordSeconds -Scene $Scene"
    try { $result.repoCommit = (& git -C $script:RepoRoot rev-parse --short HEAD 2>$null) } catch { }
    $json = Join-Path $OutDir 'result.json'
    [System.IO.File]::WriteAllText($json, ($result | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
    Write-Step "Result: $json"
}

if ($failures.Count -gt 0) { $failures | ForEach-Object { Write-Step "FAILED: $_" }; exit 1 }
$fsx = $result.frames
Write-Step ("{0}: {1} fps avg, p50 {2} ms, p99 {3} ms, 1% low {4} fps, GPU {5}%, rendered {6:N1} MP" -f $Config, $fsx.avgFps,
            $fsx.frameTimeMs.p50, $fsx.frameTimeMs.p99, $fsx.onePercentLowFps, $result.gpu.utilPct, ($result.renderedPixels / 1e6))
exit 0
