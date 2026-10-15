@echo off
rem Removes the .heproj registration made by register_heproj.cmd (current user only).
rem   /quiet   do not wait for a key at the end (for scripts and CI)
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0register_heproj.ps1" -Unregister
set "RC=%ERRORLEVEL%"
if /i not "%~1"=="/quiet" pause
exit /b %RC%
