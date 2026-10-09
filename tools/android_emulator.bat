@echo off
rem Starts the local Android emulator (AVD oe_api35) with host GPU rendering.
rem The SDK is build\android-sdk unless ANDROID_HOME is set. Then use:
rem   oe package <project> --android --install     (logs: adb logcat -s OwnEngine)
rem SwiftShader modes capture black screenshots, so -gpu host is required.
setlocal
set "SDK=%ANDROID_HOME%"
if "%SDK%"=="" set "SDK=%~dp0..\build\android-sdk"
if not exist "%SDK%\emulator\emulator.exe" (
  echo Emulator not found in %SDK%. Install it with sdkmanager: emulator platform-tools system-images;android-35;google_apis;x86_64
  exit /b 1
)
echo Starting AVD oe_api35 from %SDK%
start "" "%SDK%\emulator\emulator.exe" -avd oe_api35 -gpu host -no-snapshot-save -no-audio
echo Wait for boot: "%SDK%\platform-tools\adb.exe" shell getprop sys.boot_completed ^(prints 1^)
