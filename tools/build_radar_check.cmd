@echo off
rem Requires build.cmd first; never starts the game.
setlocal
pushd "%~dp0.."
if errorlevel 1 exit /b 1
call "%~dp0..\vsenv.cmd"
if errorlevel 1 goto fail
cl /nologo /O2 /MT /W4 /WX /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DWIN32_LEAN_AND_MEAN /Fo"build\obj\radar_capture_check.obj" "%~dp0radar_capture_check.c" /link /OUT:build\radar_capture_check.exe user32.lib gdi32.lib
if errorlevel 1 goto fail
build\radar_capture_check.exe
if errorlevel 1 goto fail
set "OBJECTS="
for %%s in (cargo_pause cloud_art d3d_fix d3d11_out diag display draw_sync frame_queue game game_folder game_speed gun_smoke smooth_balls smooth_fire smooth_radar iat idle_wait log output_window patch registry renderer saves scaling scene sha256 ship_capture ship_gpu startup_warning timer_guard wake_config wake_field wake_layer boat_oars) do call set "OBJECTS=%%OBJECTS%% build\obj\%%s.obj"
cl /nologo /O2 /MT /W4 /WX /std:c++17 /EHs-c- /GR- /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DWIN32_LEAN_AND_MEAN /Fo"build\obj\radar_check.obj" "%~dp0radar_check.cpp" %OBJECTS% /link /OUT:build\radar_check.exe advapi32.lib user32.lib comctl32.lib gdi32.lib d3d11.lib dxgi.lib
if errorlevel 1 goto fail
build\radar_check.exe
if errorlevel 1 goto fail
popd
exit /b 0
:fail
popd
exit /b 1
