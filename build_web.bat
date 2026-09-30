@echo off
rem Builds the web runtime (the player as WebAssembly + WebGL2) that
rem `oe package --web` ships: build\bin\web\oe_player.js + oe_player.wasm.
rem Needs the Emscripten SDK (https://emscripten.org/docs/getting_started/downloads.html):
rem set EMSDK=C:\path\to\emsdk (or run emsdk_env.bat first). CMake and Ninja
rem come from Visual Studio like in build.bat.
rem Usage: build_web.bat [Release^|Debug]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

where emcmake >nul 2>nul || if defined EMSDK call "%EMSDK%\emsdk_env.bat" >nul 2>nul
where emcmake >nul 2>nul || (
  echo Emscripten not found. Install emsdk and set EMSDK=C:\path\to\emsdk
  exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
where cmake >nul 2>nul || set "PATH=%PATH%;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
where ninja >nul 2>nul || set "PATH=%PATH%;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

call emcmake cmake -S "%~dp0." -B "%~dp0build-web" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DOE_BUILD_TESTS=OFF || exit /b 1
cmake --build "%~dp0build-web" --target oe_player || exit /b 1
if not exist "%~dp0build\bin\web" mkdir "%~dp0build\bin\web"
copy /y "%~dp0build-web\bin\oe_player.js" "%~dp0build\bin\web\" >nul || exit /b 1
copy /y "%~dp0build-web\bin\oe_player.wasm" "%~dp0build\bin\web\" >nul || exit /b 1
copy /y "%~dp0tools\player\web\index.html" "%~dp0build\bin\web\" >nul || exit /b 1
rem Refresh the prebuilt runtime that `oe package --web` falls back to (commit it).
copy /y "%~dp0build-web\bin\oe_player.js" "%~dp0runtime\web\" >nul || exit /b 1
copy /y "%~dp0build-web\bin\oe_player.wasm" "%~dp0runtime\web\" >nul || exit /b 1
echo.
echo Built: %~dp0build\bin\web\oe_player.js + oe_player.wasm (used by oe package --web)
