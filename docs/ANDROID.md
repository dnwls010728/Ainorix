# Android

`oe package <project> --android` builds an installable, signed APK of a game:
the player runtime (`liboe_player.so`, a `NativeActivity`) plus the game data
(`assets/game.pak`). Rendering uses OpenGL ES 3 through sokol_gfx, audio uses
AAudio, touches act as the mouse (`MouseLeft` + `mouseX/mouseY`).

## Implementation status (work log)

This feature was built across several sessions. If a session stopped
midway, continue from the first unchecked item; every checked item is
committed and pushed on the branch.

- [x] Platform layer `engine/platform/android/` (NativeActivity window, EGL/GLES3 device, touch/keys/gamepad, AAudio, lifecycle, logcat)
- [x] `CMakeLists.txt` Android branch (`liboe_player.so`) + `build_android.sh` / `build_android.bat`
- [x] Player entry point `android_main` (game data extracted from `assets/game.pak`)
- [x] `ExtractGamePak` (C++), `engine/core/Zip` (APK writer); tests pending
- [x] `oe package --android`: binary manifest + resources.arsc written by oe (no aapt2), aligned APK, apksigner (debug keystore or `--keystore`), `--install`. Verified here with apksigner verify + aapt dump (stand-in .so)
- [ ] Tests (APK writer, pak round trip) + docs (PLATFORMS.md, CLAUDE.md, READMEs)
- [ ] Prebuilt runtime `runtime/android/arm64-v8a/liboe_player.so` (needs the NDK; not available in the cloud session that wrote this)
- [ ] Tested on a device / emulator
