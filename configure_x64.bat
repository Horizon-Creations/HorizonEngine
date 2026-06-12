@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" x64
cd /d "%~dp0"
cmake --fresh . --preset x64-release
cmake --build out/build/x64-release
