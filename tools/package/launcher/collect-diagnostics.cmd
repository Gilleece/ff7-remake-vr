@echo off
rem Collects the last session's log, the settings and basic system facts into one zip next to this file,
rem to send along when something went wrong. Changes nothing.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ff7vr-launcher.ps1" diagnostics %*
exit /b %ERRORLEVEL%
