@echo off
rem Starts FINAL FANTASY VII REMAKE INTERGRADE with the VR mod, and puts the game folder back afterwards.
rem Options (after the name): -KeepInstalled  -KeepLuma  -ExtraArgs "..."  -NoPause
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ff7vr-launcher.ps1" start %*
exit /b %ERRORLEVEL%
