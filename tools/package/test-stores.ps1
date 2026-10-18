<#
.SYNOPSIS
  Tests the launcher's game lookup (launcher\ff7vr-stores.ps1) and its start through
  a store, against fake Epic Games Store and Microsoft Store / Xbox app installs.

.DESCRIPTION
  Builds a fake Epic manifests folder, fake Store packages (Get-AppxPackage is
  replaced by a function), a fake XboxGames folder and fake install folders under
  %TEMP%, and checks what the lookup finds and chooses. The Steam lookup runs
  against the real Steam install of this PC, if there is one, and is compared
  with the Steam-only lookup the launcher used before.

  The launcher's start through a store (Start-StoreGame) is taken out of
  ff7vr-launcher.ps1 without running the script, and run with the processes,
  Start-Process and the game's executable replaced by fakes: nothing is started.

  -EndToEnd also runs the launcher script itself: 'status' (changes nothing) with
  the fake installs, then 'restore' and 'start' on fake game folders (the fake
  'start' stops at the write check before anything starts). The copy used for
  'restore' and 'start' has a lock name of its own and counts only processes
  started from the fake folders, so it neither waits for nor touches a game or
  launcher that is running; the real game folder is never given to those runs.

  Nothing outside the temporary folder is changed. Exit code 0 when every check
  passed.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\package\test-stores.ps1 -EndToEnd
#>
param([switch]$EndToEnd, [switch]$Keep)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$LauncherDir = Join-Path $PSScriptRoot 'launcher'
$Launcher = Join-Path $LauncherDir 'ff7vr-launcher.ps1'
. (Join-Path $LauncherDir 'ff7vr-stores.ps1')

$script:Passed = 0
$script:Failed = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) { $script:Passed++; Write-Host "PASS  $name" }
    else { $script:Failed++; Write-Host "FAIL  $name $(if ($detail) { ": $detail" })" -ForegroundColor Red }
}
function Skip([string]$name, [string]$why) { Write-Host "SKIP  $name ($why)" -ForegroundColor Yellow }

function New-Folder([string]$p) { New-Item -ItemType Directory -Force -Path $p | Out-Null; return $p }
function New-FakeExe([string]$dir) { New-Folder $dir | Out-Null; $p = Join-Path $dir 'ff7remake_.exe'; [System.IO.File]::WriteAllBytes($p, [byte[]](0x4D, 0x5A)); return $p }
function Write-Json([string]$path, $obj) { [System.IO.File]::WriteAllText($path, ($obj | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding($false))) }
function Write-AppxManifest([string]$dir, [string]$name, [string]$appId) {
    $xml = @"
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10">
  <Identity Name="$name" Publisher="CN=Fake Publisher" Version="1.0.0.7" ProcessorArchitecture="x64" />
  <Applications>
    <Application Id="$appId" Executable="gamelaunchhelper.exe" EntryPoint="Windows.FullTrustApplication" />
  </Applications>
</Package>
"@
    [System.IO.File]::WriteAllText((Join-Path $dir 'AppxManifest.xml'), $xml, (New-Object System.Text.UTF8Encoding($false)))
}

# The Steam-only lookup the launcher had before the store lookup (for comparison).
function Get-OldSteamRoot {
    $steam = $null
    try { $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath.Replace('/', '\') } catch { }
    if (-not $steam) { return $null }
    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf) {
        foreach ($line in Get-Content -LiteralPath $vdf) { if ($line -match '^\s*"path"\s*"(.+)"\s*$') { $libs += $Matches[1].Replace('\\', '\') } }
    }
    foreach ($lib in @($libs | Select-Object -Unique)) {
        $acf = Join-Path $lib 'steamapps\appmanifest_1462040.acf'
        if (-not (Test-Path -LiteralPath $acf)) { continue }
        foreach ($line in Get-Content -LiteralPath $acf) {
            if ($line -match '^\s*"installdir"\s*"(.+)"\s*$') {
                $c = Join-Path $lib ('steamapps\common\' + $Matches[1])
                if (Test-Path -LiteralPath (Join-Path $c 'End\Binaries\Win64\ff7remake_.exe')) { return $c }
            }
        }
    }
    return $null
}

# ------------------------------------------------------------------ fake installs

$T = Join-Path ([System.IO.Path]::GetTempPath()) ('ff7vr-stores-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Folder $T | Out-Null
Write-Host "Fake installs in $T"

# Epic: the launcher's manifests and two install folders
$epicManifests = New-Folder (Join-Path $T 'Epic\Manifests')
$epicRoot = Join-Path $T 'Epic Games\FFVIIRemakeIntergrade'
New-FakeExe (Join-Path $epicRoot 'End\Binaries\Win64') | Out-Null
New-Folder (Join-Path $epicRoot '.egstore') | Out-Null
Write-Json (Join-Path $epicManifests 'A1.item') @{ DisplayName = 'Some Other Game'; InstallLocation = (Join-Path $T 'Epic Games\Other'); LaunchExecutable = 'Other.exe'; AppName = 'Other' }
Write-Json (Join-Path $epicManifests 'B2.item') @{
    DisplayName = 'FINAL FANTASY VII REMAKE INTERGRADE'; InstallLocation = $epicRoot.Replace('\', '/'); LaunchExecutable = 'ff7remake.exe'
    AppName = 'FakeAppName'; CatalogNamespace = 'fakenamespace'; CatalogItemId = 'fakeitemid'; bIsIncompleteInstall = $false
}
$epicHalf = Join-Path $T 'Epic Games\Half'
New-FakeExe (Join-Path $epicHalf 'End\Binaries\Win64') | Out-Null
Write-Json (Join-Path $epicManifests 'C3.item') @{ DisplayName = 'FINAL FANTASY VII REMAKE INTERGRADE'; InstallLocation = $epicHalf; LaunchExecutable = 'ff7remake.exe'; AppName = 'Half'; bIsIncompleteInstall = $true }
[System.IO.File]::WriteAllText((Join-Path $epicManifests 'D4.item'), '{ this is not json', (New-Object System.Text.UTF8Encoding($false)))
$env:FF7VR_EPIC_MANIFESTS = $epicManifests

# Microsoft Store: a package folder like WindowsApps, and an Xbox app folder with Content\
$pkgName = '39C668CD.FINALFANTASYVIIREMAKEINTERGRADE'
$pkgLoc = Join-Path $T ('WindowsApps\' + $pkgName + '_1.0.0.7_x64__fakepublisher')
New-FakeExe (Join-Path $pkgLoc 'End\Binaries\Win64') | Out-Null
Write-AppxManifest $pkgLoc $pkgName 'Game'
$xboxRoot = New-Folder (Join-Path $T 'X\XboxGames')
$xboxGame = Join-Path $xboxRoot 'FINAL FANTASY VII REMAKE INTERGRADE'
$xboxContent = Join-Path $xboxGame 'Content'
New-FakeExe (Join-Path $xboxContent 'End\Binaries\WinGDK') | Out-Null
Write-AppxManifest $xboxContent 'SquareEnix.FF7RemakeIntergradePC' 'FF7R'
New-Folder (Join-Path $xboxRoot 'Another Game\Content') | Out-Null
# a nested layout only the bounded search finds
$nested = Join-Path $T 'Nested\Pkg'
$nestedExe = New-FakeExe (Join-Path $nested 'Game\Sub\End\Binaries\Win64')
# a plain folder with the game in it and nothing that names a store
$plain = Join-Path $T 'Plain\FF7R'
New-FakeExe (Join-Path $plain 'End\Binaries\Win64') | Out-Null

$script:FakeAppx = @(
    [pscustomobject]@{ Name = 'Microsoft.WindowsCalculator'; Publisher = 'CN=Microsoft Corporation'; InstallLocation = (Join-Path $T 'WindowsApps\calc'); PackageFamilyName = 'Microsoft.WindowsCalculator_8wekyb3d8bbwe'; IsFramework = $false }
    [pscustomobject]@{ Name = 'Microsoft.VCLibs.140.00'; Publisher = 'CN=Microsoft Corporation'; InstallLocation = (Join-Path $T 'WindowsApps\vclibs'); PackageFamilyName = 'Microsoft.VCLibs.140.00_8wekyb3d8bbwe'; IsFramework = $true }
    [pscustomobject]@{ Name = $pkgName; Publisher = 'CN=Fake Publisher'; InstallLocation = $pkgLoc; PackageFamilyName = ($pkgName + '_fakepublisher'); IsFramework = $false }
    # installed for the user, but the Xbox app's folder is found by the folder scan
    [pscustomobject]@{ Name = 'SquareEnix.FF7RemakeIntergradePC'; Publisher = 'CN=Square Enix'; InstallLocation = ''; PackageFamilyName = 'SquareEnix.FF7RemakeIntergradePC_fakehash'; IsFramework = $false }
)
# Replace the PC's own data: functions win over cmdlets of the same name.
function Get-AppxPackage { return $script:FakeAppx }
function Get-XboxGamesRoots { return @($xboxRoot) }

$savedEnv = @{}
foreach ($n in @('FF7VR_STORE', 'FF7VR_GAME_DIR')) { $savedEnv[$n] = [Environment]::GetEnvironmentVariable($n); [Environment]::SetEnvironmentVariable($n, $null) }

try {
    # ------------------------------------------------------------------ Steam (real)
    Write-Host ''
    Write-Host '-- Steam (the real install of this PC)'
    $oldRoot = Get-OldSteamRoot
    $steam = @(Get-SteamInstalls)
    if ($oldRoot) {
        Check 'Steam: found' ($steam.Count -ge 1) "found $($steam.Count)"
        if ($steam.Count -ge 1) {
            Check 'Steam: same folder as the Steam-only lookup before' ($steam[0].Root -eq $oldRoot) "$($steam[0].Root) vs $oldRoot"
            Check 'Steam: bin is End\Binaries\Win64' ($steam[0].Bin -eq (Join-Path $oldRoot 'End\Binaries\Win64')) $steam[0].Bin
            Write-Host "      Steam install: $($steam[0].Root) ($($steam[0].Source))"
            $b = Get-ExeBuildInfo $steam[0].Exe
            Check 'Build check: the Steam exe is the known build 1.0.0.7' ($b.Known -and $b.FileVersion -eq '1.0.0.7') ("{0} 0x{1:X} 0x{2:X8} {3}" -f $b.FileVersion, $b.SizeOfImage, $b.TimeDateStamp, $b.Error)
        }
    } else {
        Skip 'Steam: real install' 'no Steam copy of the game on this PC'
    }
    $sys = Join-Path $env:SystemRoot 'System32\whoami.exe'
    $b = Get-ExeBuildInfo $sys
    Check 'Build check: another exe is not the known build' ((-not $b.Known) -and -not $b.Error) ("{0} {1}" -f $b.FileVersion, $b.Error)
    $b = Get-ExeBuildInfo (Join-Path $plain 'End\Binaries\Win64\ff7remake_.exe')
    Check 'Build check: a broken exe reports an error, not a build' ((-not $b.Known) -and [bool]$b.Error) $b.Error

    # ------------------------------------------------------------------ Epic
    Write-Host ''
    Write-Host '-- Epic Games Store (fake manifests)'
    $epic = @(Get-EpicInstalls)
    Check 'Epic: one install (other game, incomplete install and broken manifest left out)' ($epic.Count -eq 1) "found $($epic.Count): $(($epic | ForEach-Object { $_.Root }) -join '; ')"
    if ($epic.Count -ge 1) {
        $e = $epic[0]
        Check 'Epic: install folder' ($e.Root -eq $epicRoot) $e.Root
        Check 'Epic: exe folder' ($e.Bin -eq (Join-Path $epicRoot 'End\Binaries\Win64')) $e.Bin
        Check 'Epic: AppName, namespace, item id' ($e.AppName -eq 'FakeAppName' -and $e.CatalogNamespace -eq 'fakenamespace' -and $e.CatalogItemId -eq 'fakeitemid') "$($e.AppName) $($e.CatalogNamespace) $($e.CatalogItemId)"
        $uri = Get-EpicLaunchUri $e
        Check 'Epic: launch address (long form)' ($uri -eq 'com.epicgames.launcher://apps/fakenamespace%3Afakeitemid%3AFakeAppName?action=launch&silent=true') $uri
        $short = Get-EpicLaunchUri ([pscustomobject]@{ AppName = 'FakeAppName'; CatalogNamespace = ''; CatalogItemId = '' })
        Check 'Epic: launch address (app name only)' ($short -eq 'com.epicgames.launcher://apps/FakeAppName?action=launch&silent=true') $short
        Write-Host "      $uri"
    }
    # the launcher's own manifest folder when the variable is not set
    $env:FF7VR_EPIC_MANIFESTS = $null
    $d = Get-EpicManifestDir
    Check 'Epic: default manifest folder' ($d -like '*\Epic\EpicGamesLauncher\Data\Manifests') $d
    $env:FF7VR_EPIC_MANIFESTS = $epicManifests

    # ------------------------------------------------------------------ Microsoft Store / Xbox
    Write-Host ''
    Write-Host '-- Microsoft Store / Xbox app (fake packages and XboxGames folder)'
    $pk = @(Get-StoreAppxPackages)
    Check 'Xbox: package filter keeps the two game packages only' ($pk.Count -eq 2) (($pk | ForEach-Object { $_.Name }) -join ', ')
    $xb = @(Get-XboxInstalls)
    Check 'Xbox: two installs (package folder, XboxGames folder)' ($xb.Count -eq 2) "found $($xb.Count): $(($xb | ForEach-Object { $_.Exe }) -join '; ')"
    $wa = @($xb | Where-Object { $_.Root -eq $pkgLoc })
    Check 'Xbox: package folder install' ($wa.Count -eq 1) ''
    if ($wa.Count -eq 1) {
        Check 'Xbox: package family name and application id' ($wa[0].PackageFamilyName -eq ($pkgName + '_fakepublisher') -and $wa[0].AppId -eq 'Game') "$($wa[0].PackageFamilyName) $($wa[0].AppId)"
        $tg = Get-XboxLaunchTarget $wa[0]
        Check 'Xbox: start target' ($tg -eq "shell:AppsFolder\$($pkgName)_fakepublisher!Game") $tg
        Write-Host "      $tg"
    }
    $xg = @($xb | Where-Object { $_.Root -eq $xboxContent })
    Check 'Xbox: XboxGames\<game>\Content install' ($xg.Count -eq 1) (($xb | ForEach-Object { $_.Root }) -join '; ')
    if ($xg.Count -eq 1) {
        Check 'Xbox: exe in End\Binaries\WinGDK found' ($xg[0].Bin -eq (Join-Path $xboxContent 'End\Binaries\WinGDK')) $xg[0].Bin
        Check 'Xbox: family name from the package list, app id from its manifest' ($xg[0].PackageFamilyName -eq 'SquareEnix.FF7RemakeIntergradePC_fakehash' -and $xg[0].AppId -eq 'FF7R') "$($xg[0].PackageFamilyName) $($xg[0].AppId)"
    }
    $f = Find-GameExeUnder $nested
    Check 'Search: exe five folders down found' ($f -eq $nestedExe) "$f"
    Check 'Search: strict lookup does not search' ($null -eq (Find-GameExeAt $nested)) ''

    # ------------------------------------------------------------------ choosing
    Write-Host ''
    Write-Host '-- Choosing an install'
    $all = @(Get-GameInstalls)
    $stores = ($all | ForEach-Object { $_.Store }) -join ','
    $expect = $(if ($oldRoot) { 'steam,epic,xbox,xbox' } else { 'epic,xbox,xbox' })
    Check 'All installs, in the order Steam, Epic, Xbox' ($stores -eq $expect) $stores
    foreach ($i in $all) { Write-Host ("      {0,-27} {1}" -f $i.StoreName, $i.Root) }

    $r = Resolve-GameInstall
    $first = $(if ($oldRoot) { 'steam' } else { 'epic' })
    Check "Default: $first first" ($r.Install -and $r.Install.Store -eq $first) "$($r.Install.Store) $($r.Error)"
    foreach ($s in @('epic', 'xbox', 'steam')) {
        if ($s -eq 'steam' -and -not $oldRoot) { continue }
        $env:FF7VR_STORE = $s
        $r = Resolve-GameInstall
        Check "FF7VR_STORE=$s" ($r.Install -and $r.Install.Store -eq $s) "$($r.Error)"
    }
    $env:FF7VR_STORE = 'Epic '
    $r = Resolve-GameInstall
    Check 'FF7VR_STORE: case and spaces ignored' ($r.Install -and $r.Install.Store -eq 'epic') "$($r.Error)"
    $r = Resolve-GameInstall -store 'xbox'
    Check '-Store parameter wins over FF7VR_STORE' ($r.Install -and $r.Install.Store -eq 'xbox') "$($r.Error)"
    $env:FF7VR_STORE = 'gog'
    $r = Resolve-GameInstall
    Check 'FF7VR_STORE=gog refused with a message' ((-not $r.Install) -and $r.Error -match 'steam, epic or xbox') $r.Error
    Write-Host "      $($r.Error)"
    $env:FF7VR_STORE = 'epic'
    $env:FF7VR_EPIC_MANIFESTS = (New-Folder (Join-Path $T 'EmptyManifests'))
    $r = Resolve-GameInstall
    Check 'FF7VR_STORE=epic without an Epic install: error' ((-not $r.Install) -and $r.Error -match 'Epic') $r.Error
    Write-Host "      $($r.Error)"
    $env:FF7VR_EPIC_MANIFESTS = $epicManifests
    $env:FF7VR_STORE = $null

    # Without Steam (and then without Epic): the next store
    function Get-SteamRootPath { return $null }
    $r = Resolve-GameInstall
    Check 'No Steam: Epic chosen' ($r.Install -and $r.Install.Store -eq 'epic') "$($r.Install.Store)"
    $env:FF7VR_EPIC_MANIFESTS = (Join-Path $T 'EmptyManifests')
    $r = Resolve-GameInstall
    Check 'No Steam, no Epic: Microsoft Store chosen' ($r.Install -and $r.Install.Store -eq 'xbox' -and $r.Install.Root -eq $pkgLoc) "$($r.Install.Store) $($r.Install.Root)"
    $script:FakeAppx = @()
    function Get-XboxGamesRoots { return @() }
    $r = Resolve-GameInstall
    Check 'Nothing installed: error naming the three stores' ((-not $r.Install) -and $r.Error -match 'Steam, the Epic Games Store or the Microsoft Store') $r.Error
    Write-Host "      $($r.Error)"
    Remove-Item Function:\Get-SteamRootPath
    . (Join-Path $LauncherDir 'ff7vr-stores.ps1')     # back to the real Steam lookup
    function Get-AppxPackage { return $script:FakeAppx }
    function Get-XboxGamesRoots { return @($xboxRoot) }
    $script:FakeAppx = @(
        [pscustomobject]@{ Name = $pkgName; Publisher = 'CN=Fake Publisher'; InstallLocation = $pkgLoc; PackageFamilyName = ($pkgName + '_fakepublisher'); IsFramework = $false }
        [pscustomobject]@{ Name = 'SquareEnix.FF7RemakeIntergradePC'; Publisher = 'CN=Square Enix'; InstallLocation = ''; PackageFamilyName = 'SquareEnix.FF7RemakeIntergradePC_fakehash'; IsFramework = $false })
    $env:FF7VR_EPIC_MANIFESTS = $epicManifests

    # A folder given by hand
    $env:FF7VR_GAME_DIR = $epicRoot
    $r = Resolve-GameInstall
    Check 'FF7VR_GAME_DIR: Epic folder recognised, with its manifest data' ($r.Install -and $r.Install.Store -eq 'epic' -and $r.Install.AppName -eq 'FakeAppName') "$($r.Install.Store) $($r.Install.AppName) $($r.Error)"
    $env:FF7VR_GAME_DIR = $xboxContent
    $r = Resolve-GameInstall
    Check 'FF7VR_GAME_DIR: Xbox Content folder recognised' ($r.Install -and $r.Install.Store -eq 'xbox' -and $r.Install.AppId -eq 'FF7R') "$($r.Install.Store) $($r.Install.AppId) $($r.Error)"
    $env:FF7VR_GAME_DIR = $xboxGame
    $r = Resolve-GameInstall
    Check 'FF7VR_GAME_DIR: Xbox game folder (above Content) works too' ($r.Install -and $r.Install.Store -eq 'xbox' -and $r.Install.Root -eq $xboxContent) "$($r.Install.Root) $($r.Error)"
    $env:FF7VR_GAME_DIR = $plain + '\'
    $r = Resolve-GameInstall
    Check 'FF7VR_GAME_DIR: plain folder counts as Steam (as before)' ($r.Install -and $r.Install.Store -eq 'steam' -and $r.Install.Root -eq $plain) "$($r.Install.Store) $($r.Install.Root) $($r.Error)"
    $env:FF7VR_STORE = 'epic'
    $r = Resolve-GameInstall
    Check 'FF7VR_GAME_DIR + FF7VR_STORE=epic: store as given' ($r.Install -and $r.Install.Store -eq 'epic') "$($r.Install.Store)"
    $env:FF7VR_STORE = $null
    if ($oldRoot) {
        $env:FF7VR_GAME_DIR = $oldRoot
        $r = Resolve-GameInstall
        Check 'FF7VR_GAME_DIR: the real Steam folder is Steam' ($r.Install -and $r.Install.Store -eq 'steam' -and $r.Install.Root -eq $oldRoot) "$($r.Install.Store) $($r.Error)"
    }
    $env:FF7VR_GAME_DIR = (Join-Path $T 'Nothing')
    $r = Resolve-GameInstall
    Check 'FF7VR_GAME_DIR without the game: error' ((-not $r.Install) -and $r.Error -match 'No ff7remake_.exe') $r.Error
    $env:FF7VR_GAME_DIR = $null

    # ------------------------------------------------------------------ write check
    Write-Host ''
    Write-Host '-- Write check'
    $w = New-Folder (Join-Path $T 'Writable')
    Check 'Writable folder: yes' (Test-FolderWritable $w) ''
    Check 'Writable folder: no file left behind' (@(Get-ChildItem -LiteralPath $w -Force).Count -eq 0) ''
    $locked = New-Folder (Join-Path $T 'Locked')
    $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    & icacls.exe $locked /deny "*$($sid):(WD,AD)" | Out-Null
    try { Check 'Locked folder (create files denied): no' (-not (Test-FolderWritable $locked)) '' }
    finally { & icacls.exe $locked /remove:d "*$sid" | Out-Null }

    # ------------------------------------------------------------------ starting through a store
    Write-Host ''
    Write-Host '-- Start through the store (launcher functions, processes faked)'
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($Launcher, [ref]$null, [ref]$null)
    $want = @('Say', 'Info', 'Warn', 'Fail', 'Format-Procs', 'Get-GamePid', 'Wait-GamePid', 'Start-StoreGame')
    foreach ($fd in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
        if ($want -contains $fd.Name) { . ([scriptblock]::Create($fd.Extent.Text)) }
    }
    Check 'Launcher functions loaded without running the launcher' ($null -ne (Get-Command Start-StoreGame -CommandType Function -ErrorAction SilentlyContinue)) ''
    $GameProcess = 'ff7remake_'
    $QuickExitSeconds = 2; $RestartGraceSeconds = 2; $EpicRestartGraceSeconds = 3; $StoreStartSeconds = 3
    $ExtraArgs = ''
    $ViaStore = $false
    $script:Warned = $false

    # Simulated processes: a list of @{ Name; Id; From; Until } (seconds after the scenario start).
    function Get-GameProcs {
        $t = ((Get-Date) - $script:SimT0).TotalSeconds
        return @($script:Sim | Where-Object { $t -ge $_.From -and $t -lt $_.Until } | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Id = $_.Id } })
    }
    function Start-GameExe([string]$a) {
        $script:ExeStarts++
        if ($script:ExeThrows) { throw 'Access is denied' }
        $t = ((Get-Date) - $script:SimT0).TotalSeconds
        $script:Sim += @{ Name = 'ff7remake_'; Id = 4242; From = $t; Until = $t + $script:ExeLives }
        return 4242
    }
    function Start-Process {
        param([string]$FilePath, [string]$ArgumentList)
        $script:StoreCalls += , @($FilePath, $ArgumentList)
        if ($script:StoreStarts) {
            $t = ((Get-Date) - $script:SimT0).TotalSeconds
            $script:Sim += @{ Name = 'ff7remake_'; Id = 5151; From = $t + 0.5; Until = 1e9 }
        }
    }
    function Get-Process { return @() }   # no Epic Games Launcher running
    function Reset-Sim([double]$lives, [bool]$throws = $false, [bool]$storeStarts = $true) {
        $script:SimT0 = Get-Date; $script:Sim = @(); $script:ExeStarts = 0; $script:StoreCalls = @()
        $script:ExeLives = $lives; $script:ExeThrows = $throws; $script:StoreStarts = $storeStarts
    }
    $ei = $epic[0]
    $xi = $wa[0]

    Reset-Sim 1e9
    $id = Start-StoreGame $ei '-d3d11'
    Check 'Epic, the exe keeps running: its pid, the store not asked' ($id -eq 4242 -and $script:StoreCalls.Count -eq 0) "pid $id, store calls $($script:StoreCalls.Count)"

    Reset-Sim 0.5
    $id = Start-StoreGame $ei '-d3d11'
    $call = $(if ($script:StoreCalls.Count) { $script:StoreCalls[0][0] } else { '' })
    Check 'Epic, the exe closes at once: started through the launcher address' ($id -eq 5151 -and $call -eq (Get-EpicLaunchUri $ei)) "pid $id, call '$call'"

    Reset-Sim 0.5
    $script:Sim += @{ Name = 'ff7remake_'; Id = 6161; From = 1.5; Until = 1e9 }
    $id = Start-StoreGame $ei '-d3d11'
    Check 'Epic, the game starts itself again: no second start' ($id -eq 6161 -and $script:StoreCalls.Count -eq 0) "pid $id, store calls $($script:StoreCalls.Count)"

    Reset-Sim 0.5
    $script:Sim += @{ Name = 'CrashReportClient'; Id = 7171; From = 0.0; Until = 1e9 }
    $id = Start-StoreGame $ei '-d3d11'
    Check 'Epic, the exe crashed (crash reporter open): no store start' ($null -eq $id -and $script:StoreCalls.Count -eq 0) "pid $id, store calls $($script:StoreCalls.Count)"

    Reset-Sim 0 $true
    $id = Start-StoreGame $xi '-d3d11'
    $call = $(if ($script:StoreCalls.Count) { "$($script:StoreCalls[0][0]) $($script:StoreCalls[0][1])" } else { '' })
    Check 'Xbox, the exe cannot be started: explorer.exe shell:AppsFolder' ($id -eq 5151 -and $call -eq "explorer.exe $(Get-XboxLaunchTarget $xi)") "pid $id, call '$call'"

    Reset-Sim 0 $true $false
    $noPfn = New-GameInstall 'xbox' $xi.Exe 'test'
    $id = Start-StoreGame $noPfn '-d3d11'
    Check 'Xbox without a family name, game never appears: no call, gives up' ($null -eq $id -and $script:StoreCalls.Count -eq 0) "pid $id, store calls $($script:StoreCalls.Count)"

    $ViaStore = $true
    Reset-Sim 1e9
    $id = Start-StoreGame $ei '-d3d11'
    Check '-ViaStore: the exe is not started, the store is' ($id -eq 5151 -and $script:ExeStarts -eq 0 -and $script:StoreCalls.Count -eq 1) "pid $id, exe starts $($script:ExeStarts)"
    $ViaStore = $false
    foreach ($n in @('Get-GameProcs', 'Start-GameExe', 'Start-Process', 'Get-Process')) { Remove-Item "Function:\$n" }

    # ------------------------------------------------------------------ Python lookup (tools\re)
    $py = Join-Path $PSScriptRoot '..\..\.venv\Scripts\python.exe'
    if (Test-Path -LiteralPath $py) {
        Write-Host ''
        Write-Host '-- tools\re find_game_exe'
        $re = (Resolve-Path (Join-Path $PSScriptRoot '..\re')).Path
        $code = "import sys; sys.path.insert(0, r'$re'); import ff7re; print(ff7re.find_game_exe())"
        $env:FF7VR_STORE = 'epic'
        $out = (& $py -c $code 2>&1 | Out-String).Trim()
        Check 'Python: FF7VR_STORE=epic finds the fake Epic exe' ($out -eq (Join-Path $epicRoot 'End\Binaries\Win64\ff7remake_.exe')) $out
        $env:FF7VR_STORE = $null
        $env:FF7VR_GAME_DIR = $xboxGame
        $out = (& $py -c $code 2>&1 | Out-String).Trim()
        Check 'Python: FF7VR_GAME_DIR (Xbox layout)' ($out -eq (Join-Path $xboxContent 'End\Binaries\WinGDK\ff7remake_.exe')) $out
        $env:FF7VR_GAME_DIR = $null
        if ($oldRoot) {
            $out = (& $py -c $code 2>&1 | Out-String).Trim()
            Check 'Python: default is the Steam copy' ($out -eq (Join-Path $oldRoot 'End\Binaries\Win64\ff7remake_.exe')) $out
        }
    } else { Skip 'Python lookup' 'no .venv' }

    # ------------------------------------------------------------------ the launcher script itself
    if ($EndToEnd) {
        Write-Host ''
        Write-Host '-- Launcher script (separate PowerShell processes)'
        # A package folder of its own, so logs\ and settings stay in the temporary folder.
        $pkgDir = New-Folder (Join-Path $T 'package')
        foreach ($n in @('ff7vr-launcher.ps1', 'ff7vr-stores.ps1')) { Copy-Item -LiteralPath (Join-Path $LauncherDir $n) -Destination $pkgDir }
        [System.IO.File]::WriteAllBytes((Join-Path $pkgDir 'xinput1_3.dll'), [byte[]](1, 2, 3, 4))
        [System.IO.File]::WriteAllText((Join-Path $pkgDir 'ff7vr.ini'), "[stereo]`r`nenabled = 1`r`n")
        $pl = Join-Path $pkgDir 'ff7vr-launcher.ps1'
        function Invoke-Launcher([string[]]$launcherArgs) {
            $o = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $pl @launcherArgs -NoPause 2>&1 | Out-String
            return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $o }
        }

        $env:FF7VR_STORE = 'epic'
        $o = Invoke-Launcher @('status')
        Check 'status, FF7VR_STORE=epic: the fake Epic copy' ($o.Code -eq 0 -and $o.Text -match [regex]::Escape("Game folder:   $epicRoot") -and $o.Text -match 'Store:\s+Epic Games Store') "exit $($o.Code)"
        if ($oldRoot) { Check 'status, FF7VR_STORE=epic: Steam named as also installed' ($o.Text -match 'Also installed: Steam') '' }
        Write-Host ($o.Text.TrimEnd() -replace '(?m)^', '      | ')
        $env:FF7VR_STORE = $null
        $o = Invoke-Launcher @('status', '-GameDir', $xboxGame)
        Check 'status -GameDir <Xbox game folder>: Microsoft Store copy' ($o.Code -eq 0 -and $o.Text -match 'Store:\s+Microsoft Store / Xbox app') "exit $($o.Code)"
        if ($oldRoot) {
            $o = Invoke-Launcher @('status')
            Check 'status, nothing set: the Steam copy' ($o.Code -eq 0 -and $o.Text -match [regex]::Escape("Game folder:   $oldRoot") -and $o.Text -match 'Store:\s+Steam' -and $o.Text -match 'the build the mod knows') "exit $($o.Code)"
            Write-Host ($o.Text.TrimEnd() -replace '(?m)^', '      | ')
        }
        $env:FF7VR_STORE = 'gog'
        $o = Invoke-Launcher @('status')
        Check 'status, FF7VR_STORE=gog: refused, exit 1' ($o.Code -eq 1 -and $o.Text -match 'steam, epic or xbox') "exit $($o.Code)"
        $env:FF7VR_STORE = $null

        # restore and start: a copy of the launcher that sees only the fake folders. The real one
        # waits for any ff7remake_.exe and for any other launcher window.
        $iso = Get-Content -LiteralPath $pl -Raw
        $iso2 = $iso.Replace('$ours = ($n -eq $GameProcess)', '$ours = $false').Replace("'Local\ff7vr-launcher'", "'Local\ff7vr-launcher-test-$([guid]::NewGuid().ToString('N'))'")
        $patched = ($iso2 -ne $iso) -and ($iso2 -notmatch [regex]::Escape("'Local\ff7vr-launcher'")) -and ($iso2 -notmatch [regex]::Escape('$ours = ($n -eq $GameProcess)'))
        Check 'isolated launcher copy for restore and start prepared' $patched ''
        if (-not $patched) {
            Skip 'restore and start on fake folders' 'the launcher copy could not be isolated'
        } else {
            [System.IO.File]::WriteAllText($pl, $iso2, (New-Object System.Text.UTF8Encoding($false)))
            # An unfinished Epic session in the fake folder: restore puts it back.
            $eb = Join-Path $epicRoot 'End\Binaries\Win64'
            Copy-Item -LiteralPath (Join-Path $pkgDir 'xinput1_3.dll') -Destination $eb
            Copy-Item -LiteralPath (Join-Path $pkgDir 'ff7vr.ini') -Destination $eb
            [System.IO.File]::WriteAllBytes((Join-Path $eb 'dxgi.dll.vr-disabled'), [byte[]](9, 9))
            [System.IO.File]::WriteAllText((Join-Path $eb 'ff7vr.log'), 'fake log')
            $sess = [pscustomobject]@{ tool = 'ff7vr launcher'; state = 'session'; started = (Get-Date).ToString('o'); lumaSetAside = $true; files = @(
                [pscustomobject]@{ name = 'xinput1_3.dll'; sha256 = (Get-FileHash -LiteralPath (Join-Path $eb 'xinput1_3.dll') -Algorithm SHA256).Hash }
                [pscustomobject]@{ name = 'ff7vr.ini'; sha256 = (Get-FileHash -LiteralPath (Join-Path $eb 'ff7vr.ini') -Algorithm SHA256).Hash }) }
            Write-Json (Join-Path $eb 'ff7vr.session.json') $sess
            $o = Invoke-Launcher @('restore', '-GameDir', $epicRoot)
            $clean = (-not (Test-Path (Join-Path $eb 'xinput1_3.dll'))) -and (-not (Test-Path (Join-Path $eb 'ff7vr.ini'))) -and (Test-Path (Join-Path $eb 'dxgi.dll')) -and
                     (-not (Test-Path (Join-Path $eb 'dxgi.dll.vr-disabled'))) -and (-not (Test-Path (Join-Path $eb 'ff7vr.session.json'))) -and
                     (@(Get-ChildItem -LiteralPath (Join-Path $pkgDir 'logs') -Filter 'ff7vr.log' -Recurse -File -ErrorAction SilentlyContinue).Count -eq 1)
            Check 'restore -GameDir <Epic folder>: mod removed, dxgi.dll back, log kept' ($o.Code -eq 0 -and $clean) "exit $($o.Code)"
            Write-Host ($o.Text.TrimEnd() -replace '(?m)^', '      | ')

            # A locked Xbox folder: start stops before changing anything, with the instruction.
            $xbBin = Join-Path $xboxContent 'End\Binaries\WinGDK'
            $before = @(Get-ChildItem -LiteralPath $xbBin -Force | ForEach-Object { $_.Name }) -join ','
            & icacls.exe $xbBin /deny "*$($sid):(WD,AD)" | Out-Null
            try { $o = Invoke-Launcher @('start', '-GameDir', $xboxGame, '-NoIdleWait') }
            finally { & icacls.exe $xbBin /remove:d "*$sid" | Out-Null }
            $after = @(Get-ChildItem -LiteralPath $xbBin -Force | ForEach-Object { $_.Name }) -join ','
            Check 'start on a locked Xbox folder: refused with the advanced-management instruction' ($o.Code -eq 1 -and $o.Text -match 'Enable\s+advanced management features' -and $o.Text -match 'Nothing was changed') "exit $($o.Code)"
            Check 'start on a locked Xbox folder: folder unchanged, nothing started' ($before -eq $after -and $o.Text -notmatch 'Starting the game') "$before -> $after"
            Write-Host ($o.Text.TrimEnd() -replace '(?m)^', '      | ')
        }
    }
} finally {
    foreach ($n in $savedEnv.Keys) { [Environment]::SetEnvironmentVariable($n, $savedEnv[$n]) }
    $env:FF7VR_EPIC_MANIFESTS = $null
    if (-not $Keep) { Remove-Item -LiteralPath $T -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
Write-Host "$script:Passed passed, $script:Failed failed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
