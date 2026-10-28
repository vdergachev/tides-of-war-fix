@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1
call "%~dp0vsenv.cmd"
if errorlevel 1 goto fail
set "LLVM=%VSROOT%\VC\Tools\Llvm\x64\bin"
if not exist "%LLVM%\clang-format.exe" goto fail
if not exist "%LLVM%\clang-tidy.exe" goto fail
for /r src %%f in (*.c *.cpp *.h) do (
    "%LLVM%\clang-format.exe" --dry-run -Werror "%%f"
    if errorlevel 1 goto fail
)
for /r src %%f in (*.c) do (
    "%LLVM%\clang-tidy.exe" "%%f" -- --target=i686-pc-windows-msvc -fms-extensions -fms-compatibility -fasm-blocks -std=c11 -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DWIN32_LEAN_AND_MEAN -DNDEBUG
    if errorlevel 1 goto fail
)
for /r src %%f in (*.cpp) do (
    "%LLVM%\clang-tidy.exe" "%%f" --checks="-*,clang-analyzer-*,bugprone-use-after-move,bugprone-sizeof-expression,readability-isolate-declaration,readability-braces-around-statements,readability-misleading-indentation" -- --target=i686-pc-windows-msvc -fms-extensions -fms-compatibility -std=c++17 -fno-exceptions -fno-rtti -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DWIN32_LEAN_AND_MEAN -DNDEBUG
    if errorlevel 1 goto fail
)
popd
exit /b 0
:fail
echo lint failed. 1>&2
popd
exit /b 1
