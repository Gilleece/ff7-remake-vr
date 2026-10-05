@echo off
rem Starts FINAL FANTASY VII REMAKE INTERGRADE with the VR mod installed in this folder.
rem It starts the game executable directly, as the launcher does: a start through Steam
rem currently crashes with the Steam overlay (see ff7vr-docs\GUIDE.md). Steam must be running.
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
start "" /D "%~dp0" "%~dp0ff7remake_.exe" -d3d11 %*
