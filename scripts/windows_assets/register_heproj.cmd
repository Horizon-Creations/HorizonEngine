@echo off
rem Registers .heproj project files with the Horizon Editor for the current user.
rem Launcher only: the work is in register_heproj.ps1 next to this file. It is a .cmd
rem so a double-click runs it (a .ps1 would open in an editor) and so the script
rem execution policy, which a downloaded ZIP's files fall under, does not get in the way.
rem   /quiet   do not wait for a key at the end (for scripts and CI)
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0register_heproj.ps1"
set "RC=%ERRORLEVEL%"
if /i not "%~1"=="/quiet" pause
exit /b %RC%
