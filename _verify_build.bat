@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cmake --build "C:\Users\conno\source\repos\Horizon-Creations\HorizonEngine\.claude\worktrees\opt-src-he-editor-2\out\build\x64-release" --target he_tests -j 8
