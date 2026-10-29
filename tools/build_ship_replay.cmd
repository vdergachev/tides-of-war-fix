@echo off
setlocal
pushd "%~dp0.."
if errorlevel 1 exit /b 1
call "%~dp0..\vsenv.cmd"
if errorlevel 1 goto fail
if not exist build\obj mkdir build\obj
cl /nologo /O2 /MT /W4 /WX /std:c++17 /EHs-c- /GR- /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DWIN32_LEAN_AND_MEAN /Fo"build\obj\\" "%~dp0ship_replay.cpp" src\dll\ship_gpu.cpp /link /OUT:build\ship_replay.exe /SUBSYSTEM:CONSOLE,6.01 d3d11.lib d3dcompiler.lib
if errorlevel 1 goto fail
popd
exit /b 0
:fail
popd
exit /b 1
