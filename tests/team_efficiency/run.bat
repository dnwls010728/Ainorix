@echo off
setlocal
for %%I in ("%~dp0..\..") do set ROOT=%%~fI
set BUILD=%ROOT%\build-team-efficiency

rem Match build.bat: a plain cmd.exe does not have MSVC's standard include/lib paths.
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (
  echo Visual Studio with C++ tools not found. Install "Desktop development with C++".
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set CMAKE=cmake
where cmake >nul 2>nul || set CMAKE="%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

%CMAKE% -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES="%~dp0Inject.cmake" || exit /b 1
%CMAKE% --build "%BUILD%" --target oe_team_efficiency_tests oe || exit /b 1
ctest --test-dir "%BUILD%" -R oe_team_efficiency_tests --output-on-failure || exit /b 1
