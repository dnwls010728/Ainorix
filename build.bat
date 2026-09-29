@echo off
rem Builds OwnEngine with MSVC + Ninja (both ship with Visual Studio 2022).
rem Usage: build.bat [Debug|Release]   (default Release)
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (
  echo Visual Studio with C++ tools not found. Install "Desktop development with C++".
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set CMAKE=cmake
where cmake >nul 2>nul || set CMAKE="%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

rem A running `oe mcp` / `oe editor` locks oe.exe. Windows allows renaming a
rem running executable, so move it aside and clean up old copies when free.
set BIN=%~dp0build\bin
if exist "%BIN%\oe.old*.exe" del /q "%BIN%\oe.old*.exe" >nul 2>nul
if exist "%BIN%\oe.exe" move /y "%BIN%\oe.exe" "%BIN%\oe.old%RANDOM%.exe" >nul 2>nul

%CMAKE% -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
%CMAKE% --build "%~dp0build" || exit /b 1
echo.
echo Built: %~dp0build\bin\oe.exe
