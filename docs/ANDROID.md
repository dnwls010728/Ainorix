# Android

`oe package <project> --android` builds an installable, signed APK of a game:
the player runtime (`liboe_player.so`, a `NativeActivity` with no Java code)
plus the game data (`assets/game.pak`). Rendering uses OpenGL ES 3 through
sokol_gfx (the same `GpuRenderer` as desktop and web), audio uses AAudio,
touches act as the mouse. Android 8.0 (API 26) or newer.

```bat
build_android.bat                                   :: once: liboe_player.so for arm64-v8a + x86_64 (needs the NDK)
build\bin\oe.exe package MyGame --android           :: dist\MyGame-android\MyGame.apk (debug key)
build\bin\oe.exe package MyGame --android --install :: + adb install and start on the connected phone / emulator
build\bin\oe.exe package MyGame --android --aab --keystore my.jks --ks-pass env:KS_PASS --key-alias mygame
                                                    :: dist\MyGame-android\MyGame.aab for Google Play
adb logcat -s OwnEngine                             :: engine logs and Lua errors from the device
```

## What you need

| For | Needs | Where from |
|---|---|---|
| Building the runtime (`build_android.bat` / `./build_android.sh`), once and after engine C++ changes | Android NDK, CMake + Ninja | Android Studio > SDK Manager > SDK Tools > *NDK (Side by side)*; CMake/Ninja from Visual Studio (like `build.bat`) or *CMake* in the same SDK Manager list |
| Signing (`oe package --android`) | APK: `apksigner` (SDK build-tools) and Java; `.aab`: only Java (`jarsigner` is part of the JDK) | Android Studio (bundles both; `oe` finds its JDK) or the SDK command-line tools + `sdkmanager "build-tools;35.0.0"` and JDK 17+ |
| `--install` | `adb` (SDK platform-tools) + USB debugging on the phone, or an emulator | Android Studio |

oe writes the APK itself (binary `AndroidManifest.xml`, `resources.arsc`,
alignment): **no aapt2, android.jar, Gradle or Android project** is involved.
The SDK is found through `--sdk <folder>`, `ANDROID_HOME`, `ANDROID_SDK_ROOT`
or Android Studio's default location (`%LOCALAPPDATA%\Android\Sdk`,
`~/Android/Sdk`, `~/Library/Android/sdk`); Java through `JAVA_HOME`, Android
Studio's `jbr` folder or the `PATH`; the NDK through `ANDROID_NDK_HOME` or the
newest `ndk\` folder of the SDK.

The runtime does not depend on the game: build it once, package any number
of games. `oe package --android` looks for it in `build/bin/android/<abi>/`
(a local build) and then `runtime/android/<abi>/` (a prebuilt runtime
committed to the repository, like `runtime/web/`), and packs every ABI it
finds (`--abi arm64-v8a` limits it). ABIs: `arm64-v8a` (phones and tablets),
`x86_64` (emulator), `armeabi-v7a` (old 32-bit phones:
`build_android.bat Release "arm64-v8a armeabi-v7a x86_64"`).

## Options

`project.json` (all optional):

```json
"android": {
  "package": "com.yourname.mygame",   // application id; default com.ownengine.<name>
  "versionCode": 1,                   // integer, raise it for every store upload
  "versionName": "1.0",
  "orientation": "landscape",         // landscape | portrait | auto; default from window width/height
  "icon": "assets/icon.png"           // square PNG (192x192 or 512x512); default: a generated play-button icon
}
```

The launcher label is `window.title` (else `name`).

Command line (overrides project.json): `--package com.x.y`, `--version-code N`,
`--version-name S`, `--orientation landscape|portrait|auto`, `--abi a,b`,
`--out dist/Name-android`, `--sdk <folder>`, `--debuggable` (lets
`adb shell run-as <package>` read the app's files), `--unsigned` (skip signing,
writes `Name-unsigned.apk`), `--install` (adb install -r + start), `--aab` (App Bundle instead of an APK).

### Google Play: App Bundles

`--aab` writes `Name.aab`: the same game in the bundle format Play requires for new apps
(`BundleConfig.pb` + a `base/` module whose manifest and resource table are in aapt2's protobuf
format, written by oe like the APK's binary files). It is signed with `jarsigner` using your
**upload key** (`--keystore ... --key-alias ...`); Play App Signing then signs the APKs it delivers to
phones. Without `--keystore` the bundle gets the debug key, which Play rejects — fine for checking it
with [bundletool](https://developer.android.com/tools/bundletool):
`bundletool build-apks --bundle=MyGame.aab --output=MyGame.apks --mode=universal` then
`bundletool install-apks --apks=MyGame.apks`. Raise `versionCode` for every upload.

### Signing

Without `--keystore` the APK is signed with the Android debug key
(`~/.android/debug.keystore`, created with keytool if missing — the same one
Android Studio uses). That installs on any phone and emulator, but Google Play
needs your own key:

```bat
keytool -genkeypair -keystore my-release.jks -alias mygame -keyalg RSA -keysize 2048 -validity 10000
set KS_PASS=...
build\bin\oe.exe package MyGame --android --keystore my-release.jks --ks-pass env:KS_PASS --key-alias mygame
```

`--ks-pass` / `--key-pass` accept `env:VAR`, `file:path`, `pass:text` or plain
text (apksigner's formats). Keep the keystore and its password safe: updates
of a published app must be signed with the same key. Google Play takes new
apps only as App Bundles (`--aab`, below); APKs are for sideloading, other
stores and testing.

## How the game runs on the device

- `assets/game.pak` (the same format as the web build) is unpacked into the
  app's private files folder on the first start after each install/update
  (`assets/game.id` changes with the data), then the engine opens it like a
  desktop project folder.
- **Input**: every finger is in `input.touches()`; the first one is also
  `MouseLeft` at `mouseX/mouseY` (so `UIButton`, `onClick` and pointer events
  work with touch). On-screen controls: `UIButton {key: "Left"}` holds that key
  while any finger is on it, several buttons at once ([UI.md](UI.md)). While
  the game calls `input.lockMouse`, dragging produces `mouseDX/mouseDY` (touch look).
  The Back button is `Escape` (the activity does not close by itself).
  Keyboards use the usual key names; gamepads: D-pad / left stick = arrows,
  A = `Space`, B = `Escape`, X = `Shift`, Y = `Control`, Start = `Enter`.
  Games for phones need on-screen controls (`UIButton.key`) for anything beyond taps.
- **Screen**: immersive fullscreen (status and navigation bars hidden; a swipe
  from the edge shows them briefly), drawn into the display cutout, kept on while the game runs; rotation and resizing
  (split screen, foldables) do not restart the game. `window.renderScale`
  lowers the 3D resolution on slow GPUs, as on desktop. `window.renderer:
  "software"` draws with the CPU renderer instead (slow, for testing).
- **Lifecycle**: in the background the game is frozen (no simulation, audio
  paused) and resumes where it was; the GL context is kept, only the window
  surface is recreated.
- **Logs**: engine logs (stderr) and errors go to logcat with the tag
  `OwnEngine`.
- The engine is compiled with `-ffp-contract=off` (no fused multiply-add on
  ARM), so the simulation stays bit-identical to the desktop and web builds.

## Code

- `engine/platform/android/PlatformAndroid.cpp` — window, input, AAudio, lifecycle, logcat, JNI (open URL).
- `engine/platform/android/GpuAndroid.cpp` — EGL context + window surface for sokol_gfx GLES3.
- `engine/platform/android/AndroidApp.h` — glue between the platform files and `android_main`.
- `tools/player/main.cpp` — `android_main` (unpacks the game, runs the same loop as desktop).
- `engine/app/AndroidPackage.cpp` — binary XML manifest, resource table, APK layout (tested in `AndroidApk`).
- `engine/core/Zip.cpp` — zip reader/writer with alignment.
- `tools/oe/main.cpp` (`CmdPackageAndroid`) — finds the runtime, SDK and Java, signs, installs.
- `CMakeLists.txt` (`if(ANDROID)`), `build_android.sh`, `build_android.bat`.

## Implementation status (work log)

This feature was built across several sessions. If a session stopped
midway, continue from the first unchecked item; every checked item is
committed and pushed on the branch.

- [x] Platform layer `engine/platform/android/` (NativeActivity window, EGL/GLES3 device, touch/keys/gamepad, AAudio, lifecycle, logcat)
- [x] `CMakeLists.txt` Android branch (`liboe_player.so`) + `build_android.sh` / `build_android.bat`
- [x] Player entry point `android_main` (game data extracted from `assets/game.pak`)
- [x] `ExtractGamePak` (C++), `engine/core/Zip` (APK writer)
- [x] `oe package --android`: binary manifest + resources.arsc written by oe (no aapt2), aligned APK, apksigner (debug keystore or `--keystore`), `--install`. Verified with `apksigner verify` + `aapt dump badging/xmltree/resources` using a stand-in .so
- [x] Tests (`AndroidGamePakExtract`, `ZipWriterAlignment`, `AndroidApk`) + docs
- [x] **Compile the runtime with the real NDK** (`build_android.bat`): NDK r28c (28.2.13676358), Release, API 26, `arm64-v8a` and `x86_64`. Fixed two existing Clang warnings; checked exported NativeActivity entry point and 16 KB ELF load-segment alignment.
- [x] Prebuilt `runtime/android/<abi>/liboe_player.so` for both ABIs and `runtime/android/README.md`, so packaging needs no NDK. Real-library APK signing/alignment and AAB signing verified.
- [x] Android 15 x86_64 emulator with WHPX and `-gpu host`: start, touch, `UIButton.key` press/release and `onClick`, portrait/landscape rotation, Home + return (same PID and script state), Back = Escape, logcat. AAudio opened at 48 kHz stereo; AudioFlinger showed an active track and pause/resume.
- [ ] Physical-device checks: arm64 hardware, simultaneous fingers, speaker/headphone playback and reconnect, 16 KB device execution. The emulator's host audio driver could not initialize, so audible output is unverified.
- [x] Android `oe_tests`: 58 tests passed on the emulator. CMake now builds the Android test executable with `OE_BUILD_TESTS=ON`; `OE_TEST_SOURCE_DIR` points at staged fixtures. Two GPU tests skip without a NativeActivity window; four native editor tests are not built on Android.
- [x] Immersive mode: `oe_ANativeActivity_onCreate` (manifest `android.app.func_name`) hooks a pipe into the UI thread's looper; JNI `setSystemUiVisibility` + `layoutInDisplayCutoutMode` run there
- [x] Multi-touch: `InputState.touches`, Lua `input.touches()`, API `input.touch`, `UIButton.key` (held by any finger/mouse = key down), Android fills every pointer
- [x] Rebuild `runtime/web/` with Emscripten 6.0.10, including `UIButton.key` and the runtime fixes. `BINARYEN_CORES=1` avoids an optimizer crash on this Windows host.
- [ ] Fill `input.touches` on the web platform (the current web touch handler only maps the first finger to the mouse).
- [x] `.aab` (`oe package --android --aab`): proto manifest + `resources.pb` + `BundleConfig.pb`, jarsigner; validated with bundletool (`bundletool validate`, `build-apks --mode=universal`)

### Verification notes (2026-10-01)

The SDK and NDK were downloaded from Google's official repository and their
archive SHA-1 checksums verified. Build-tools 35.0.0 and emulator 37.3.2 were
used with the AOSP API 35 x86_64 image (revision 2). Local toolchains, images,
test projects and signing keys stay in ignored build folders.

`samples/Hello` was packaged with both real ABIs. `apksigner verify --verbose`,
`aapt dump badging`, `zipalign -c -P 16 -v 4` and `jarsigner -verify` passed.
The test signing certificate is self-signed; this is not a Play upload test.

A temporary template game with a Lua probe and `UIButton {key: "Left"}`
confirmed `input.touches`, `onClick`, held/released keys and Escape through
logcat. Its tick counter stopped while Home was active, resumed without
`onStart` running again, and survived rotation and window surface recreation.
Landscape and portrait screenshots were inspected. AAudio pause/resume was
also visible in AudioFlinger's track history.

The unused selection mask pass produced GLES framebuffer errors after the
MSAA scene pass. Games now skip that pass when there is no highlighted entity;
the composite binds a valid fallback texture. The GPU regression test checks
selection removal and restoration on reused render targets.

Android testing also exposed consecutive Lua API writes with identical file
timestamps. `script.write` now forces the changed module to reload while
preserving instance state. Test fixtures can use a device source directory
and no longer require a writable POSIX `/tmp`. Windows-hosted WebAssembly
tests also required drive-path handling and a lexical relative-path fallback.
Final suites passed: Windows 62 tests, Android 58 tests, and WebAssembly/Node
58 tests. Packaging with the local Android runtime temporarily set aside
confirmed the committed `runtime/android/` fallback signs, installs and starts.
Final suites passed: Windows 62 tests, Android 58 tests, and WebAssembly/Node
58 tests. Packaging with the local Android runtime temporarily set aside
confirmed the committed `runtime/android/` fallback signs, installs and starts.

- [ ] Investigate emulator SwiftShader/SwiftShader indirect black output.
  These modes started without Lua/GPU errors after the mask change but still
  captured black screenshots. Use `-gpu host` for the verified emulator path.

To repeat the Android test suite, configure an NDK build with
`-DOE_BUILD_TESTS=ON`, build `oe_tests`, then stage the fixtures:

```sh
adb shell mkdir -p /data/local/tmp/oe-tests/third_party
adb push build-android/x86_64/bin/oe_tests samples templates AGENTS.md CLAUDE.md /data/local/tmp/oe-tests/
adb push third_party/fonts /data/local/tmp/oe-tests/third_party/
adb shell 'cd /data/local/tmp/oe-tests && chmod +x oe_tests && OE_TEST_SOURCE_DIR=/data/local/tmp/oe-tests ./oe_tests'
```

Run from the repository root. Keep the working directory at the staged root
so the engine can find `templates/`; test output is written beneath its
`build/` folder. Add `adb -s <serial>` to select a device when several are connected.

## Not done yet

- An analog on-screen joystick component (buttons with `key` cover digital controls).
- Options for the immersive mode / cutout (always on) and per-ABI bundle splits tuning.
- Adaptive icons (`mipmap-anydpi` foreground/background), splash screen.
- Compressed APK entries (everything is stored; game data that is already PNG/OGG gains little).
