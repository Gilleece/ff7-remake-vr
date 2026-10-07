@echo off
rem Starts FINAL FANTASY VII REMAKE INTERGRADE with the VR mod installed in this folder.
rem It starts the game executable directly with -d3d11. Starting the game from Steam
rem works as well (see ff7vr-docs\GUIDE.md). Steam must be running.
rem Extra arguments are passed on to the game.
setlocal
set SteamAppId=1462040
set SteamGameId=1462040
if not exist "%~dp0ff7remake_.exe" (
    echo ff7remake_.exe was not found next to this file. Put the mod's files into the game's
    echo End\Binaries\Win64 folder and start this file from there.
    pause
    exit /b 1
)
rem A game started again soon after quitting can stay slow (about 10 frames per second) for
rem minutes. When the mod's last log was written less than 90 s ago, wait until 90 s have passed.
set "FF7VR_DIR=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$d = $env:FF7VR_DIR; $t = @(); $l = Join-Path $d 'ff7vr.log'; if (Test-Path -LiteralPath $l) { $t += (Get-Item -LiteralPath $l).LastWriteTime }; $a = Join-Path $d 'ff7vr-logs'; if (Test-Path -LiteralPath $a) { $t += @(Get-ChildItem -LiteralPath $a -Filter 'ff7vr-*.log' -File | ForEach-Object { $_.LastWriteTime }) }; if ($t.Count -eq 0) { exit 0 }; $s = ((Get-Date) - ($t | Sort-Object)[-1]).TotalSeconds; if ($s -lt 0 -or $s -ge 90) { exit 0 }; Write-Host ('The previous session ended ' + [int]$s + ' s ago. Waiting ' + [int](90 - $s) + ' s before starting the game: a game started again soon after quitting can stay slow for minutes.'); Start-Sleep -Milliseconds ([int]((90 - $s) * 1000))"
start "" /D "%~dp0" "%~dp0ff7remake_.exe" -d3d11 %*
