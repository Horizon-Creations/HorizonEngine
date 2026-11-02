@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cmake --preset x64-release -DHE_SDK_RELEASE_SET=OFF -DDEPLOY_DIR=%~dp0out/deploy
