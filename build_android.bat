@echo off
rem Builds the Android runtime (the player as liboe_player.so, a NativeActivity)
rem that `oe package --android` puts into APKs:
rem   build\bin\android\<abi>\liboe_player.so  (and runtime\android\<abi>\, commit it)
rem Needs the Android NDK (Android Studio: SDK Manager ^> SDK Tools ^> NDK), found
rem through ANDROID_NDK_HOME or the newest ndk\ folder of the SDK (ANDROID_HOME,
rem ANDROID_SDK_ROOT, %LOCALAPPDATA%\Android\Sdk). CMake and Ninja come from
rem Visual Studio like in build.bat, or from the SDK's cmake\ folder.
rem Usage: build_android.bat [Release^|Debug] ["ABIs", default "arm64-v8a x86_64"]
rem   arm64-v8a = phones and tablets, x86_64 = emulator, armeabi-v7a = old 32-bit phones
setlocal enabledelayedexpansion
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release
set ABIS=%~2
if "%ABIS%"=="" set ABIS=arm64-v8a x86_64

set SDK=%ANDROID_HOME%
if "%SDK%"=="" set SDK=%ANDROID_SDK_ROOT%
if "%SDK%"=="" set SDK=%LOCALAPPDATA%\Android\Sdk
set NDK=%ANDROID_NDK_HOME%
if "%NDK%"=="" set NDK=%ANDROID_NDK_ROOT%
if "%NDK%"=="" if exist "%SDK%\ndk" for /f "usebackq tokens=*" %%d in (`dir /b /ad /on "%SDK%\ndk"`) do set NDK=%SDK%\ndk\%%d
if not exist "%NDK%\build\cmake\android.toolchain.cmake" (
  echo Android NDK not found. Install it in Android Studio ^(SDK Manager ^> SDK Tools ^> NDK^) or set ANDROID_NDK_HOME.
  exit /b 1
)
echo NDK: %NDK%

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath 2^>nul`) do set VSDIR=%%i
where cmake >nul 2>nul || if defined VSDIR set "PATH=%PATH%;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
where ninja >nul 2>nul || if defined VSDIR set "PATH=%PATH%;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if exist "%SDK%\cmake" for /f "usebackq tokens=*" %%d in (`dir /b /ad /on "%SDK%\cmake"`) do set SDKCMAKE=%SDK%\cmake\%%d\bin
where ninja >nul 2>nul || if defined SDKCMAKE set "PATH=%PATH%;%SDKCMAKE%"
where cmake >nul 2>nul || (
  echo CMake not found. Install Visual Studio 2022 ^(build.bat^) or CMake in Android Studio's SDK Manager.
  exit /b 1
)

for %%a in (%ABIS%) do (
  cmake -S "%~dp0." -B "%~dp0build-android\%%a" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DOE_BUILD_TESTS=OFF ^
    -DCMAKE_TOOLCHAIN_FILE="%NDK%\build\cmake\android.toolchain.cmake" ^
    -DANDROID_ABI=%%a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static || exit /b 1
  cmake --build "%~dp0build-android\%%a" --target oe_player || exit /b 1
  if not exist "%~dp0build\bin\android\%%a" mkdir "%~dp0build\bin\android\%%a"
  if not exist "%~dp0runtime\android\%%a" mkdir "%~dp0runtime\android\%%a"
  copy /y "%~dp0build-android\%%a\bin\liboe_player.so" "%~dp0build\bin\android\%%a\" >nul || exit /b 1
  rem Refresh the prebuilt runtime that `oe package --android` falls back to (commit it).
  copy /y "%~dp0build-android\%%a\bin\liboe_player.so" "%~dp0runtime\android\%%a\" >nul || exit /b 1
)
echo.
echo Built: %~dp0build\bin\android\^<abi^>\liboe_player.so (used by oe package --android)
