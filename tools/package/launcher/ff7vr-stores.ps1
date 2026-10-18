# Finds FINAL FANTASY VII REMAKE INTERGRADE on Steam, the Epic Games Store and the
# Microsoft Store / Xbox app. Dot-sourced by ff7vr-launcher.ps1 (the player's launcher)
# and by tools\dev\common.ps1 (the dev harness). Windows PowerShell 5.1, StrictMode 2.
# Only reads: nothing here changes a file, the registry or a process.
#
# An install is a record:
#   Store       'steam' | 'epic' | 'xbox'
#   StoreName   'Steam' | 'Epic Games Store' | 'Microsoft Store / Xbox app'
#   Root        the game's install folder (the one holding End\)
#   Bin         the folder holding ff7remake_.exe (normally Root\End\Binaries\Win64)
#   Exe         full path of ff7remake_.exe
#   Source      how it was found, for messages
#   AppName, CatalogNamespace, CatalogItemId     Epic only (from the launcher's manifest)
#   PackageName, PackageFamilyName, AppId        Microsoft Store / Xbox only
#
# Choosing one (Resolve-GameInstall): the folder given (-GameDir or FF7VR_GAME_DIR) wins;
# otherwise the store named by FF7VR_STORE (steam | epic | xbox); otherwise the first
# found in the order Steam, Epic Games Store, Microsoft Store / Xbox app.
#
# The functions Get-EpicManifestDir, Get-StoreAppxPackages and Get-XboxGamesRoots are
# the only places that look at the PC; tools\package\test-stores.ps1 replaces them with
# fakes. FF7VR_EPIC_MANIFESTS (a folder of .item files) replaces the Epic launcher's
# manifest folder, for tests.

$script:StoreSteamAppId   = 1462040
$script:StoreLookupErrors = @()     # set by Get-GameInstalls: stores whose lookup failed
$script:StoreGameExe      = 'ff7remake_.exe'
$script:StoreOrder        = @('steam', 'epic', 'xbox')
$script:StoreNames        = @{ steam = 'Steam'; epic = 'Epic Games Store'; xbox = 'Microsoft Store / Xbox app' }
# Where the exe sits below the install folder. WinGDK is where Unreal puts the binaries of
# a Microsoft Store (GDK) build; whether this game's Store build does so is not known.
$script:StoreExeRelPaths = @('End\Binaries\Win64\ff7remake_.exe', 'End\Binaries\WinGDK\ff7remake_.exe')
# The build the mod knows (src/engine stereo_abi.h): Steam, file version 1.0.0.7.
$script:KnownBuild = @{ FileVersion = '1.0.0.7'; SizeOfImage = 0x5EFC000; TimeDateStamp = 0x698BA49C }
# Names that mark the game in store data (display names, package names, folder names).
$script:StoreGamePattern = '(?i)final\s*fantasy\s*(vii|7)\s*remake|ffviir|ff7r|ff7\s*remake|finalfantasy(vii|7)remake|ffvii\s*remake'

function Get-StoreName([string]$store) {
    if ($script:StoreNames.ContainsKey($store)) { return $script:StoreNames[$store] }
    return $store
}

# A property of a JSON object or an Appx record, or $null when it has none (StrictMode-safe).
function Get-PropValue($o, [string]$name) {
    if ($null -eq $o) { return $null }
    $p = $o.PSObject.Properties[$name]
    if ($p) { return $p.Value }
    return $null
}

function New-GameInstall([string]$store, [string]$exe, [string]$source, [hashtable]$extra = @{}) {
    $exe = [System.IO.Path]::GetFullPath($exe)
    $bin = Split-Path -Parent $exe
    $root = $bin
    # <root>\End\Binaries\<platform>\ff7remake_.exe
    $binaries = Split-Path -Parent $bin
    $end = Split-Path -Parent $binaries
    if ((Split-Path -Leaf $binaries) -ieq 'Binaries' -and (Split-Path -Leaf $end) -ieq 'End') { $root = Split-Path -Parent $end }
    $o = [ordered]@{
        Store = $store; StoreName = (Get-StoreName $store); Root = $root; Bin = $bin; Exe = $exe; Source = $source
        AppName = ''; CatalogNamespace = ''; CatalogItemId = ''
        PackageName = ''; PackageFamilyName = ''; AppId = ''
    }
    foreach ($k in $extra.Keys) { $o[$k] = [string]$extra[$k] }
    return [pscustomobject]$o
}

# ff7remake_.exe at one of the usual places below $base (also below $base\Content, the
# Xbox app's layout). Returns the full path or $null.
function Find-GameExeAt([string]$base) {
    if (-not $base) { return $null }
    foreach ($b in @($base, (Join-Path $base 'Content'))) {
        foreach ($rel in $script:StoreExeRelPaths) {
            $p = Join-Path $b $rel
            if (Test-Path -LiteralPath $p -PathType Leaf) { return $p }
        }
    }
    return $null
}

# As Find-GameExeAt, and when not there, a search for ff7remake_.exe down to
# <base>\<a>\<b>\End\Binaries\<platform>. For store folders only (never a whole drive).
function Find-GameExeUnder([string]$base) {
    $p = Find-GameExeAt $base
    if ($p) { return $p }
    if (-not $base -or -not (Test-Path -LiteralPath $base -PathType Container)) { return $null }
    $hit = @(Get-ChildItem -LiteralPath $base -Filter $script:StoreGameExe -File -Recurse -Depth 5 -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($hit.Count -gt 0) { return $hit[0].FullName }
    return $null
}

# ------------------------------------------------------------------ Steam

function Get-SteamRootPath {
    try {
        $p = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath
        if ($p) { return $p.Replace('/', '\') }
    } catch { }
    return $null
}

# Steam's library folders: Steam's own folder and every "path" in libraryfolders.vdf.
function Get-SteamLibraryFolders {
    $steam = Get-SteamRootPath
    if (-not $steam) { return @() }
    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf) {
        foreach ($line in Get-Content -LiteralPath $vdf) {
            if ($line -match '^\s*"path"\s*"(.+)"\s*$') { $libs += $Matches[1].Replace('\\', '\') }
        }
    }
    return @($libs | Select-Object -Unique)
}

function Get-SteamInstalls {
    $out = @()
    foreach ($lib in Get-SteamLibraryFolders) {
        $acf = Join-Path $lib ("steamapps\appmanifest_{0}.acf" -f $script:StoreSteamAppId)
        if (-not (Test-Path -LiteralPath $acf)) { continue }
        foreach ($line in Get-Content -LiteralPath $acf) {
            if ($line -match '^\s*"installdir"\s*"(.+)"\s*$') {
                $dir = Join-Path $lib ('steamapps\common\' + $Matches[1])
                $exe = Find-GameExeAt $dir
                if ($exe) { $out += New-GameInstall 'steam' $exe "Steam library $lib (app $script:StoreSteamAppId)" }
            }
        }
    }
    return $out
}

# ------------------------------------------------------------------ Epic Games Store

# The Epic Games Launcher's manifest folder (one .item file per installed game).
function Get-EpicManifestDir {
    if ($env:FF7VR_EPIC_MANIFESTS) { return $env:FF7VR_EPIC_MANIFESTS }
    foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\Epic Games\EpicGamesLauncher', 'HKLM:\SOFTWARE\Epic Games\EpicGamesLauncher')) {
        try {
            $d = (Get-ItemProperty -Path $k -Name AppDataPath -ErrorAction Stop).AppDataPath
            if ($d) {
                $m = Join-Path $d 'Manifests'
                if (Test-Path -LiteralPath $m) { return $m }
            }
        } catch { }
    }
    $pd = $env:ProgramData
    if (-not $pd) { $pd = 'C:\ProgramData' }
    return (Join-Path $pd 'Epic\EpicGamesLauncher\Data\Manifests')
}

function Get-EpicInstalls {
    $out = @()
    $dir = Get-EpicManifestDir
    if (-not $dir -or -not (Test-Path -LiteralPath $dir)) { return $out }
    foreach ($f in @(Get-ChildItem -LiteralPath $dir -Filter '*.item' -File -ErrorAction SilentlyContinue)) {
        $m = $null
        try { $m = Get-Content -LiteralPath $f.FullName -Raw -ErrorAction Stop | ConvertFrom-Json } catch { continue }
        $name = [string](Get-PropValue $m 'DisplayName')
        $launch = [string](Get-PropValue $m 'LaunchExecutable')
        $loc = [string](Get-PropValue $m 'InstallLocation')
        $isGame = ($name -match $script:StoreGamePattern) -or ($launch -match '(?i)(^|[\\/])ff7remake_?\.exe$')
        if (-not $isGame -or -not $loc) { continue }
        if ([bool](Get-PropValue $m 'bIsIncompleteInstall')) { continue }
        $loc = $loc.Replace('/', '\')
        $exe = $null
        if ($launch -match '(?i)(^|[\\/])ff7remake_\.exe$') {
            $p = Join-Path $loc $launch.Replace('/', '\')
            if (Test-Path -LiteralPath $p -PathType Leaf) { $exe = $p }
        }
        if (-not $exe) { $exe = Find-GameExeUnder $loc }
        if (-not $exe) { continue }
        $out += New-GameInstall 'epic' $exe "Epic Games Launcher manifest $($f.Name)" @{
            AppName = [string](Get-PropValue $m 'AppName')
            CatalogNamespace = [string](Get-PropValue $m 'CatalogNamespace')
            CatalogItemId = [string](Get-PropValue $m 'CatalogItemId')
        }
    }
    return $out
}

# The address that asks the Epic Games Launcher to start the game. The long form
# (namespace:item:app) is what the launcher's own desktop shortcuts use; the short form
# (app name only) when the manifest lacks the other two.
function Get-EpicLaunchUri($inst) {
    if (-not $inst.AppName) { return $null }
    $id = $inst.AppName
    if ($inst.CatalogNamespace -and $inst.CatalogItemId) {
        $id = "$($inst.CatalogNamespace)%3A$($inst.CatalogItemId)%3A$($inst.AppName)"
    }
    return "com.epicgames.launcher://apps/$id`?action=launch&silent=true"
}

# ------------------------------------------------------------------ Microsoft Store / Xbox app

# Installed packages of the current user that may be the game (Square Enix's, or named like
# it). The game itself is confirmed later by its exe. Returns records with Name, Publisher,
# InstallLocation, PackageFamilyName.
function Get-StoreAppxPackages {
    $pk = @()
    try { $pk = @(Get-AppxPackage -ErrorAction Stop) } catch { return @() }
    return @($pk | Where-Object {
        -not $_.IsFramework -and (($_.Name -match $script:StoreGamePattern) -or ($_.Name -match '(?i)square.?enix|^39C668CD\.') -or ($_.Publisher -match '(?i)square.?enix'))
    })
}

# Folders the Xbox app installs games into: <drive>:\XboxGames, and the folder named in a
# drive's .GamingRoot file when it names another one.
function Get-XboxGamesRoots {
    $out = @()
    foreach ($d in @([System.IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Fixed' -and $_.IsReady })) {
        $r = $d.RootDirectory.FullName
        $x = Join-Path $r 'XboxGames'
        if (Test-Path -LiteralPath $x -PathType Container) { $out += $x }
        $gr = Join-Path $r '.GamingRoot'
        if (Test-Path -LiteralPath $gr -PathType Leaf) {
            try {
                # "RGBX", a count, then UTF-16 folder names relative to the drive.
                $bytes = [System.IO.File]::ReadAllBytes($gr)
                if ($bytes.Length -gt 8) {
                    $text = [System.Text.Encoding]::Unicode.GetString($bytes, 8, $bytes.Length - 8)
                    foreach ($n in ($text -split "`0")) {
                        $n = $n.Trim().TrimStart('\')
                        if (-not $n -or $n -match '[:*?"<>|]') { continue }
                        $p = Join-Path $r $n
                        if ((Test-Path -LiteralPath $p -PathType Container) -and ($out -notcontains $p)) { $out += $p }
                    }
                }
            } catch { }
        }
    }
    return $out
}

# Identity name and first application id from the package's AppxManifest.xml.
function Get-AppxManifestInfo([string]$folder) {
    foreach ($f in @($folder, (Split-Path -Parent $folder))) {
        if (-not $f) { continue }
        $p = Join-Path $f 'AppxManifest.xml'
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { continue }
        try {
            [xml]$x = Get-Content -LiteralPath $p -Raw -ErrorAction Stop
            $name = ''; $app = ''
            $id = $x.SelectSingleNode("//*[local-name()='Identity']")
            if ($id -and $id.Attributes['Name']) { $name = $id.Attributes['Name'].Value }
            $a = $x.SelectSingleNode("//*[local-name()='Applications']/*[local-name()='Application']")
            if ($a -and $a.Attributes['Id']) { $app = $a.Attributes['Id'].Value }
            return [pscustomobject]@{ Name = $name; AppId = $app; Path = $p }
        } catch { }
    }
    return $null
}

function Get-XboxInstalls {
    $out = @()
    $seen = @{}
    $packages = @(Get-StoreAppxPackages)
    foreach ($pk in $packages) {
        $loc = [string](Get-PropValue $pk 'InstallLocation')
        if (-not $loc) { continue }
        $exe = Find-GameExeUnder $loc
        if (-not $exe) { continue }
        $mi = Get-AppxManifestInfo $loc
        $out += New-GameInstall 'xbox' $exe "Microsoft Store package $(Get-PropValue $pk 'PackageFamilyName')" @{
            PackageName = [string](Get-PropValue $pk 'Name')
            PackageFamilyName = [string](Get-PropValue $pk 'PackageFamilyName')
            AppId = $(if ($mi) { $mi.AppId } else { '' })
        }
        $seen[[System.IO.Path]::GetFullPath($exe).ToLowerInvariant()] = $true
    }
    # Games the Xbox app installed into an XboxGames folder: <root>\<game>\Content\...
    foreach ($r in @(Get-XboxGamesRoots)) {
        foreach ($g in @(Get-ChildItem -LiteralPath $r -Directory -ErrorAction SilentlyContinue)) {
            $exe = Find-GameExeAt $g.FullName
            if (-not $exe -and $g.Name -match $script:StoreGamePattern) { $exe = Find-GameExeUnder $g.FullName }
            if (-not $exe) { continue }
            $key = [System.IO.Path]::GetFullPath($exe).ToLowerInvariant()
            if ($seen.ContainsKey($key)) { continue }
            $seen[$key] = $true
            $content = Join-Path $g.FullName 'Content'
            if (-not (Test-Path -LiteralPath $content)) { $content = $g.FullName }
            $mi = Get-AppxManifestInfo $content
            $pfn = ''; $pname = ''
            if ($mi -and $mi.Name) {
                $pname = $mi.Name
                $pk = @($packages | Where-Object { (Get-PropValue $_ 'Name') -eq $mi.Name } | Select-Object -First 1)
                if ($pk.Count -gt 0) { $pfn = [string](Get-PropValue $pk[0] 'PackageFamilyName') }
            }
            $out += New-GameInstall 'xbox' $exe "Xbox app folder $($g.FullName)" @{
                PackageName = $pname; PackageFamilyName = $pfn; AppId = $(if ($mi) { $mi.AppId } else { '' })
            }
        }
    }
    return $out
}

# What explorer.exe starts for a Store app: shell:AppsFolder\<family name>!<application id>.
function Get-XboxLaunchTarget($inst) {
    if (-not $inst.PackageFamilyName -or -not $inst.AppId) { return $null }
    return "shell:AppsFolder\$($inst.PackageFamilyName)!$($inst.AppId)"
}

# ------------------------------------------------------------------ all stores

# Every install found, in the order Steam, Epic, Microsoft Store / Xbox. A store whose
# lookup fails is skipped (its error goes to $script:StoreLookupErrors).
function Get-GameInstalls {
    $script:StoreLookupErrors = @()
    $out = @()
    foreach ($s in $script:StoreOrder) {
        try {
            switch ($s) {
                'steam' { $out += @(Get-SteamInstalls) }
                'epic'  { $out += @(Get-EpicInstalls) }
                'xbox'  { $out += @(Get-XboxInstalls) }
            }
        } catch { $script:StoreLookupErrors += "$(Get-StoreName $s): $_" }
    }
    # The same folder found twice (an override pointing at a store folder, two Steam entries).
    $seen = @{}
    $uniq = @()
    foreach ($i in $out) {
        $k = $i.Exe.ToLowerInvariant()
        if ($seen.ContainsKey($k)) { continue }
        $seen[$k] = $true
        $uniq += $i
    }
    return $uniq
}

# Which store a folder given by hand belongs to, from what is in it.
function Get-StoreOfFolder([string]$root, [string]$exe) {
    if (Test-Path -LiteralPath (Join-Path $root '.egstore')) { return 'epic' }
    if ($exe -match '(?i)\\WindowsApps\\|\\XboxGames\\|\\WinGDK\\') { return 'xbox' }
    foreach ($b in @($root, (Split-Path -Parent $root))) {
        if (-not $b) { continue }
        if ((Test-Path -LiteralPath (Join-Path $b 'AppxManifest.xml')) -or (Test-Path -LiteralPath (Join-Path $b 'MicrosoftGame.config'))) { return 'xbox' }
    }
    return 'steam'
}

# Picks the install to use. Returns @{ Install; All; Error; Note }:
#   Install  the chosen record, or $null (then Error says why)
#   All      every install found (empty when a folder was given by hand)
#   Note     how the choice was made
function Resolve-GameInstall([string]$gameDir = '', [string]$store = '') {
    if (-not $store -and $env:FF7VR_STORE) { $store = $env:FF7VR_STORE }
    $store = ([string]$store).Trim().ToLowerInvariant()
    if ($store -eq 'microsoft' -or $store -eq 'msstore' -or $store -eq 'gamepass') { $store = 'xbox' }
    $res = @{ Install = $null; All = @(); Error = ''; Note = '' }
    if ($store -and $script:StoreOrder -notcontains $store) {
        $res.Error = "FF7VR_STORE is '$store'; it can be steam, epic or xbox (or empty)."
        return $res
    }
    if (-not $gameDir -and $env:FF7VR_GAME_DIR) { $gameDir = $env:FF7VR_GAME_DIR }
    if ($gameDir) {
        $gameDir = $gameDir.Trim().Trim('"').TrimEnd('\')
        $exe = Find-GameExeAt $gameDir
        if (-not $exe -and (Test-Path -LiteralPath (Join-Path $gameDir $script:StoreGameExe) -PathType Leaf)) { $exe = Join-Path $gameDir $script:StoreGameExe }
        if (-not $exe) {
            $res.Error = "No ff7remake_.exe in the folder given ($gameDir): it should be the game's install folder, the one that contains End\."
            return $res
        }
        $s = $store
        if (-not $s) { $s = Get-StoreOfFolder $gameDir $exe }
        # A store's own record (with what starting through the store needs) when the folder is one it knows.
        $match = $null
        try {
            $key = [System.IO.Path]::GetFullPath($exe).ToLowerInvariant()
            $known = @(Get-GameInstalls | Where-Object { $_.Store -eq $s -and $_.Exe.ToLowerInvariant() -eq $key })
            if ($known.Count -gt 0) { $match = $known[0] }
        } catch { }
        if ($match) { $match.Source = "the folder given ($($match.Source))"; $res.Install = $match }
        else { $res.Install = New-GameInstall $s $exe 'the folder given' }
        $res.Note = $(if ($store) { "folder given, store $store (FF7VR_STORE)" } else { 'folder given' })
        return $res
    }
    $all = @(Get-GameInstalls)
    $res.All = $all
    if ($store) {
        $pick = @($all | Where-Object { $_.Store -eq $store })
        if ($pick.Count -eq 0) {
            $res.Error = "FF7VR_STORE is '$store', but no $(Get-StoreName $store) install of the game was found."
            return $res
        }
        $res.Install = $pick[0]
        $res.Note = "FF7VR_STORE = $store"
        return $res
    }
    if ($all.Count -eq 0) {
        $res.Error = 'FINAL FANTASY VII REMAKE INTERGRADE was not found on Steam, the Epic Games Store or the Microsoft Store / Xbox app.'
        return $res
    }
    $res.Install = $all[0]
    $res.Note = $(if (@($all | Select-Object -ExpandProperty Store -Unique).Count -gt 1) { 'several stores; Steam first, then Epic, then Microsoft Store / Xbox' } else { 'found' })
    return $res
}

# ------------------------------------------------------------------ checks

# $true when files can be created and deleted in the folder (a Store package folder
# usually refuses that).
function Test-FolderWritable([string]$dir) {
    $p = Join-Path $dir ('ff7vr-write-test-{0}.tmp' -f [guid]::NewGuid().ToString('N').Substring(0, 8))
    try {
        $fs = [System.IO.File]::Open($p, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write)
        $fs.Dispose()
        [System.IO.File]::Delete($p)
        return $true
    } catch {
        try { if (Test-Path -LiteralPath $p) { [System.IO.File]::Delete($p) } } catch { }
        return $false
    }
}

# File version, SizeOfImage and PE time stamp of an exe, and whether that is the build the
# mod knows (the mod itself checks SizeOfImage and the time stamp).
function Get-ExeBuildInfo([string]$exe) {
    $info = [pscustomobject]@{ FileVersion = ''; SizeOfImage = 0; TimeDateStamp = 0; Known = $false; Error = '' }
    try {
        $vi = (Get-Item -LiteralPath $exe -ErrorAction Stop).VersionInfo
        $info.FileVersion = '{0}.{1}.{2}.{3}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart
        $fs = [System.IO.File]::Open($exe, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
        try {
            $buf = New-Object byte[] 4096
            $n = $fs.Read($buf, 0, $buf.Length)
            if ($n -lt 0x40 -or $buf[0] -ne 0x4D -or $buf[1] -ne 0x5A) { throw 'not a PE file' }
            $pe = [BitConverter]::ToInt32($buf, 0x3C)
            if ($pe -le 0 -or $pe + 0x58 -gt $n -or [BitConverter]::ToUInt32($buf, $pe) -ne 0x00004550) { throw 'no PE header' }
            $info.TimeDateStamp = [BitConverter]::ToUInt32($buf, $pe + 8)
            # optional header starts at pe + 24; SizeOfImage at +56 for both PE32 and PE32+
            $info.SizeOfImage = [BitConverter]::ToUInt32($buf, $pe + 24 + 56)
        } finally { $fs.Dispose() }
        $info.Known = ($info.SizeOfImage -eq $script:KnownBuild.SizeOfImage -and $info.TimeDateStamp -eq $script:KnownBuild.TimeDateStamp)
    } catch { $info.Error = "$_" }
    return $info
}
