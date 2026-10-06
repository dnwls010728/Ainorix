@echo off
rem Builds the OnlineRPG back end with MSVC + Ninja (both ship with Visual Studio 2022):
rem   build\bin\oe_login.exe, oe_channel.exe, oe_server_tests.exe   (next to oe.exe; build files in build\server)
rem Usage: build.bat [Debug|Release]   (default Release)
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set ROOT=%~dp0..\..\..

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (
  echo Visual Studio with C++ tools not found. Install "Desktop development with C++".
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set CMAKE=cmake
where cmake >nul 2>nul || set CMAKE="%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

%CMAKE% -S "%~dp0." -B "%ROOT%\build\server" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=%ROOT%\build\bin" || exit /b 1
%CMAKE% --build "%ROOT%\build\server" || exit /b 1
echo.
echo Built: build\bin\oe_login.exe, oe_channel.exe, oe_server_tests.exe
