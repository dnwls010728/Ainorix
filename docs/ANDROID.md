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

Enabled `project.json` networking (`network.mode` other than `none`) adds
`android.permission.INTERNET` to both APK and App Bundle manifests. Single-player/none
projects keep their previous permissions. M4 lobbies/RPC require a rebuilt Android runtime;
device networking is unverified without the NDK/device. See [NETWORK_NOTES.md](NETWORK_NOTES.md).

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
  They also expose the portable `Gamepad*` button names and six analog axes
  through `input.axis`, with a shared 0.15 dead zone and disconnect/focus reset
  ([INPUT.md](INPUT.md)); physical-controller verification remains outstanding.
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

## Status

Both ABIs (arm64-v8a, x86_64) are built with NDK r28c and committed in `runtime/android/` (sizes and hashes in
its README). `oe package --android` APKs and App Bundles pass `apksigner verify`, `aapt2 dump badging` and
`zipalign -c -P 16 4`.

- [ ] Run on a physical device: arm64 hardware, simultaneous fingers, speaker/headphone playback and
      reconnect, 16 KB page-size execution, lifecycle, and the features newer than the last emulator run
      (Light2D, shader graphs, fog/grading, host bridge, `input.viewSize`, networking).
- [ ] Text entry: the soft keyboard and `KeyCharacterMap` text path is compiled but not run. Composed text
      (Korean/Japanese IMEs, swipe typing) needs a Java view with an `InputConnection` (a dex in the APK).
- [ ] Emulator SwiftShader modes capture black screenshots; use `-gpu host`.

Emulator: `tools\android_emulator.bat` starts an AVD named `oe_api35` with `-gpu host` (SDK from `ANDROID_HOME`,
else `build\android-sdk`; create the AVD once with `avdmanager create avd -n oe_api35 -k
"system-images;android-35;google_apis;x86_64" -d pixel_7`). Then `oe package <project> --android --install` and
`adb exec-out screencap -p > shot.png`. Last run 2026-10-10 (API 35, x86_64, WHPX): the default template and
`games/RPG` render (shader graphs, shadows, DOF, fog, Korean UI text) with no `gpu` warnings, and keyboard input
moves the RPG hero. `games/RPG` has no touch controls yet.

To repeat the test suite on Android, configure an NDK build with `-DOE_BUILD_TESTS=ON`, build `oe_tests`,
then stage it with its fixtures:

```sh
adb shell mkdir -p /data/local/tmp/oe-tests/third_party /data/local/tmp/oe-tests/tests /data/local/tmp/oe-tests/games
adb push build-android/x86_64/bin/oe_tests templates AGENTS.md CLAUDE.md /data/local/tmp/oe-tests/
adb push tests/fixtures /data/local/tmp/oe-tests/tests/
adb push games/RPG /data/local/tmp/oe-tests/games/
adb push third_party/fonts /data/local/tmp/oe-tests/third_party/
adb shell 'cd /data/local/tmp/oe-tests && chmod +x oe_tests && OE_TEST_SOURCE_DIR=/data/local/tmp/oe-tests ./oe_tests'
```

Add `adb -s <serial>` to select a device when several are connected.

## Not done yet

- Options for the immersive mode / cutout (always on) and per-ABI bundle splits tuning.
- Adaptive icons (`mipmap-anydpi` foreground/background), splash screen.
- Compressed APK entries (everything is stored; game data that is already PNG/OGG gains little).






