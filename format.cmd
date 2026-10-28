@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1
call "%~dp0vsenv.cmd"
if errorlevel 1 goto fail
set "LLVM=%VSROOT%\VC\Tools\Llvm\x64\bin"
if not exist "%LLVM%\clang-format.exe" goto fail
for /r src %%f in (*.c *.cpp *.h) do (
    "%LLVM%\clang-format.exe" -i "%%f"
    if errorlevel 1 goto fail
)
popd
exit /b 0
:fail
echo format failed. 1>&2
popd
exit /b 1
