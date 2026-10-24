@echo off
rem Called without setlocal so the compiler environment reaches the caller.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
set "VSROOT="
if not exist "%~dp0build\obj" mkdir "%~dp0build\obj"
if errorlevel 1 exit /b 1
"%VSWHERE%" -latest -products * -property installationPath >"%~dp0build\obj\vswhere.txt"
if errorlevel 1 exit /b 1
for /f "usebackq delims=" %%i in ("%~dp0build\obj\vswhere.txt") do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
rem Prefer the SDK used in development; fall back to the default SDK (CI images may differ).
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86 10.0.26100.0 >nul 2>nul
if not errorlevel 1 exit /b 0
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1
exit /b 0
