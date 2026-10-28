@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1
call "%~dp0vsenv.cmd"
if errorlevel 1 goto fail

if not exist build\obj mkdir build\obj
if errorlevel 1 goto fail
set "FLAGS=/nologo /O2 /MT /W4 /WX- /GS /Zi /DNDEBUG /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DWIN32_LEAN_AND_MEAN"
cl %FLAGS% /std:c++17 /EHs-c- /GR- /c /Fo"build\obj\\" /Fd"build\obj\render.pdb" src\dll\d3d11_out.cpp src\dll\scene_gpu.cpp src\dll\cloud_art.cpp src\dll\frame_queue.cpp src\dll\output_window.cpp src\dll\renderer.cpp src\dll\wake_layer.cpp src\dll\wake_field.cpp
if errorlevel 1 goto fail
cl %FLAGS% /LD /Fo"build\obj\\" /Fd"build\obj\dll.pdb" src\dll\main.c src\dll\log.c src\dll\diag.c src\dll\patch.c src\dll\game_folder.c src\dll\game.c src\dll\saves.c src\dll\registry.c src\dll\iat.c src\dll\scaling.c src\dll\display.c src\dll\d3d_fix.c build\obj\d3d11_out.obj src\dll\scene.c build\obj\scene_gpu.obj build\obj\cloud_art.obj build\obj\frame_queue.obj build\obj\output_window.obj build\obj\renderer.obj build\obj\wake_layer.obj build\obj\wake_field.obj src\dll\wake_config.c src\dll\startup_warning.c src\dll\game_speed.c src\dll\draw_sync.c src\dll\idle_wait.c src\dll\timer_guard.c src\common\sha256.c /link /IMPLIB:build\obj\tow_fix.lib /OUT:build\tow_fix.dll /PDB:build\tow_fix.pdb /DEF:src\dll\tow_fix.def /SUBSYSTEM:WINDOWS,6.01 /MACHINE:X86 /DEBUG /INCREMENTAL:NO advapi32.lib user32.lib gdi32.lib d3d11.lib dxgi.lib
if errorlevel 1 goto fail
cl %FLAGS% /Fo"build\obj\\" /Fd"build\obj\patcher.pdb" src\patcher\tow_patch.c src\common\sha256.c /link /OUT:build\tow_patch.exe /PDB:build\tow_patch.pdb /SUBSYSTEM:CONSOLE,6.01 /MACHINE:X86 /DEBUG /INCREMENTAL:NO advapi32.lib user32.lib
if errorlevel 1 goto fail
copy /y tow_fix.ini build\tow_fix.ini >nul
if errorlevel 1 goto fail
popd
exit /b 0
:fail
echo Build failed. 1>&2
popd
exit /b 1
