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
adb logcat -s OwnEngine                             :: engine logs and Lua errors from the device
```

## What you need

| For | Needs | Where from |
|---|---|---|
| Building the runtime (`build_android.bat` / `./build_android.sh`), once and after engine C++ changes | Android NDK, CMake + Ninja | Android Studio > SDK Manager > SDK Tools > *NDK (Side by side)*; CMake/Ninja from Visual Studio (like `build.bat`) or *CMake* in the same SDK Manager list |
| Signing (`oe package --android`) | `apksigner` (SDK build-tools) and Java | Android Studio (bundles both; `oe` finds its JDK) or the SDK command-line tools + `sdkmanager "build-tools;35.0.0"` and JDK 17+ |
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
writes `Name-unsigned.apk`), `--install` (adb install -r + start).

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
apps only as Android App Bundles (.aab), which oe does not build yet (see
*Not done yet*); APKs are for sideloading, other stores and testing.

## How the game runs on the device

- `assets/game.pak` (the same format as the web build) is unpacked into the
  app's private files folder on the first start after each install/update
  (`assets/game.id` changes with the data), then the engine opens it like a
  desktop project folder.
- **Input**: the first finger is `MouseLeft` at `mouseX/mouseY` (so `UIButton`,
  `onClick`, `input.click` and pointer events work with touch); while the game
  calls `input.lockMouse`, dragging produces `mouseDX/mouseDY` (touch look).
  The Back button is `Escape` (the activity does not close by itself).
  Keyboards use the usual key names; gamepads: D-pad / left stick = arrows,
  A = `Space`, B = `Escape`, X = `Shift`, Y = `Control`, Start = `Enter`.
  Games for phones need on-screen controls (UI buttons) for anything beyond taps.
- **Screen**: fullscreen, kept on while the game runs; rotation and resizing
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
- [ ] **Compile the runtime with the real NDK** (`build_android.bat`). The cloud session that wrote this had no NDK (its download was blocked), so the Android C++ files were only syntax-checked against the NDK's `android/*.h` headers with stand-ins for `native_window.h`, `AAudio.h` and `android_native_app_glue.h`. Small compile fixes may be needed there.
- [ ] Commit the prebuilt runtime `runtime/android/<abi>/liboe_player.so` (+ a `runtime/android/README.md` like `runtime/web/README.md`) so packaging needs no NDK
- [ ] Test on a device / emulator: start, touch → UI buttons, rotation, Home + return (surface recreation), Back = Escape, audio, logcat
- [ ] Run `oe_tests` on a device (would need test data paths that do not use `OE_SOURCE_DIR`)
- [x] Immersive mode: `oe_ANativeActivity_onCreate` (manifest `android.app.func_name`) hooks a pipe into the UI thread's looper; JNI `setSystemUiVisibility` + `layoutInDisplayCutoutMode` run there
- [x] Multi-touch: `InputState.touches`, Lua `input.touches()`, API `input.touch`, `UIButton.key` (held by any finger/mouse = key down), Android fills every pointer
- [ ] Rebuild the web runtime (`build_web.bat`): the committed one predates `UIButton.key`, so web builds of scenes that use it fail to load until then. The web platform could also fill `input.touches` (Emscripten touch events list every finger)
- [ ] `.aab` (`oe package --android --aab`): proto manifest + `resources.pb` + `BundleConfig.pb`, jarsigner; validated with bundletool (`bundletool validate`, `build-apks --mode=universal`)

## Not done yet

- Android App Bundle (`.aab`) for new Google Play apps — needs `bundletool`; the APK layout here is the base module.
- Multi-touch (only the first finger is the pointer), an on-screen joystick component.
- Immersive mode (hiding the navigation bar needs a Java call on the UI thread), display cutout settings.
- Adaptive icons (`mipmap-anydpi` foreground/background), splash screen.
- Compressed APK entries (everything is stored; game data that is already PNG/OGG gains little).
