@echo off
rem Puts the game folder back to normal: removes the VR mod's files and puts ReShade/Luma's dxgi.dll back.
rem Safe to run at any time; does nothing when nothing needs doing.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ff7vr-launcher.ps1" restore %*
exit /b %ERRORLEVEL%
