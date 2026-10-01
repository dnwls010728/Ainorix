# macOS and iOS (plan)

Status: **planned, not started**. This document is the plan and the work log
for the macOS and iOS ports, written so that any agent can pick it up. Start
from the first unchecked line of the work log at the bottom (see
[DESIGN.md](DESIGN.md) §5).

Goal, matching the other platforms ([PLATFORMS.md](PLATFORMS.md)):

```bash
oe package MyGame --macos   # dist/MyGame-macos/MyGame.app (+ .zip), signed and notarized when a key is given
oe package MyGame --ios     # dist/MyGame-ios/MyGame.ipa, signed with a provisioning profile
oe editor MyGame            # the native editor on macOS
```

## Constraint: no Mac on the maintainer's side

The maintainer has no Mac. The plan is built around that:

| Step | Needs a Mac? | How |
|---|---|---|
| Write the platform code (Objective-C++ `.mm`) | No | Any machine; it is only compiled on macOS |
| Metal shaders | No | `sokol-shdc` runs on Windows/Linux and emits `metal_macos` / `metal_ios` |
| Compile and run `oe_tests` | Yes (CI) | GitHub Actions `macos-latest` runner. Public repo: free; private repo: macOS minutes cost 10x |
| Build the player runtimes | Yes (CI) | Same workflow; the result is committed to `runtime/macos/` and `runtime/ios/` like `runtime/web/` and `runtime/android/` |
| Write `.app` / `.ipa` | No | Plain folder/zip layout + `Info.plist` (XML) written by `oe`, like the Android APK |
| Code signing + notarization | No | [`rcodesign`](https://github.com/indygreg/apple-platform-rs) (apple-codesign) runs on Windows/Linux. Needs an Apple Developer account ($99/year): Developer ID certificate (macOS), distribution certificate + provisioning profile (iOS), App Store Connect API key (notarization) |
| Run in the iOS Simulator, screenshot | Yes (CI) | `xcrun simctl boot/install/launch`, `xcrun simctl io booted screenshot` on the runner |
| App Store upload | Yes (CI) | `xcrun altool` / Transporter are macOS-only |
| Feel on hardware (Retina DPI, touch, notch safe area, audio latency, Metal on a real GPU) | Yes (device) | One session on a borrowed or rented Mac (e.g. AWS EC2 Mac) and an iPhone at the end |

Do not use osxcross: the Xcode SDK license only allows building on Apple
hardware. Everything that needs the SDK runs on the GitHub macOS runner.

## Design

### Prebuilt runtimes (packaging needs no Mac)

Same model as the web and Android players: the runtime does not depend on the
game, so it is built once in CI and committed.

- `runtime/macos/oe_player` — universal binary (`arm64` + `x86_64`,
  `CMAKE_OSX_ARCHITECTURES`), minimum macOS 11.
- `runtime/ios/oe_player` — `arm64` device binary, minimum iOS 15. Optionally
  `runtime/ios-simulator/oe_player` for Simulator packages.
- `runtime/macos/README.md`, `runtime/ios/README.md` — toolchain version,
  commit, how it was built (like `runtime/web/README.md`).

`oe package --macos` / `--ios` looks in `build/bin/<platform>/` first and then
`runtime/<platform>/` (the Android rule).

### Platform layer

- `engine/platform/apple/` — code shared by both: `GpuMetal.mm`
  (`SOKOL_METAL` + `SOKOL_IMPL`, a `GpuDevice` on a `CAMetalLayer`; readback
  through a blit into a shared `MTLBuffer`), `AudioApple.mm` (CoreAudio
  `AudioUnit` / `AVAudioEngine`, 48 kHz stereo float from the existing mixer),
  time (`mach_absolute_time`), `PlatformUserLanguage` (`NSLocale`),
  `PlatformOpenUrl`.
- `engine/platform/macos/PlatformMacOS.mm` — AppKit `NSWindow` + `NSView`
  with a `CAMetalLayer`, keyboard/mouse to `InputState`, `PumpEvents` for
  games, **editor event mode** (`SetEventMode`/`TakeEvents`: every key as
  `WindowKey`, text through `NSTextInputClient` incl. IME, all mouse buttons,
  wheel/trackpad, focus, close, file drops), `SetCursor`, `DpiScale`
  (`backingScaleFactor`), `Maximize`. `win32/PlatformWin32.cpp` is the
  reference; [PLATFORMS.md](PLATFORMS.md) "Porting checklist" item 5 lists
  what the editor needs.
- `engine/platform/ios/PlatformIOS.mm` — UIKit app delegate + view controller
  with a `CAMetalLayer` view, the game loop on `CADisplayLink`, multi-touch
  into `InputState.touches` (first finger also as the mouse, like Android),
  lifecycle (background = pause audio and simulation), safe area insets,
  orientation from `project.json`. Game data is read from the app bundle's
  `game/` folder (no extraction needed, unlike Android).
- `.mm` files are Objective-C++; engine code outside `engine/platform/*` stays
  portable C++17 (DESIGN.md invariants).

### Build

- `CMakeLists.txt`: before the final `else()` add
  `elseif(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")` -> `ios` and
  `elseif(APPLE)` -> `macos`; `enable_language(OBJCXX)`; link `Metal`,
  `QuartzCore`, `AppKit`/`UIKit`, `AudioToolbox`, `AVFoundation`; compile
  `oe_editor` with `SOKOL_METAL` (next to the `SOKOL_D3D11` / `SOKOL_GLES3`
  branches around line 280). Today macOS falls into `null`, which must keep
  working (`-DOE_PLATFORM=null` override for the CLI-only build).
- `build_macos.sh`, `build_ios.sh` — mirror `build_android.sh`.
- `tools/shaders/compile_shaders.sh` / `.bat`: add `metal_macos:metal_ios` to
  `--slang`, regenerate and commit `Shaders.glsl.h` (needs `sokol-shdc` only,
  no Mac).
- `.github/workflows/apple.yml` — `macos-latest`: build (null first, then
  macos), run `oe_tests`, build the runtimes and upload them as artifacts; an
  iOS job builds for the Simulator, boots one, installs a packaged
  `samples/Hello`, takes a screenshot and uploads it. Whether the runner VM
  exposes a Metal device is unknown: if not, GPU tests must skip cleanly
  (as on Android without a window) and the software renderer is used.

### Packaging (`tools/oe/main.cpp`, next to `CmdPackageAndroid`)

- macOS `.app`: `Name.app/Contents/{Info.plist, MacOS/Name, Resources/game/,
  Resources/AppIcon.icns}`. `Info.plist` keys: `CFBundleIdentifier`,
  `CFBundleName`, `CFBundleExecutable`, `CFBundleShortVersionString`,
  `CFBundleVersion`, `LSMinimumSystemVersion`, `NSHighResolutionCapable`.
  Zip it for distribution (keep the executable bit; `engine/core/Zip` must
  write Unix mode in the external attributes).
- iOS `.ipa`: `Payload/Name.app/{Info.plist, Name, game/, icons}` zipped.
  Extra keys: `UIRequiredDeviceCapabilities=[metal]`,
  `UISupportedInterfaceOrientations`, `UILaunchScreen`,
  `MinimumOSVersion`, `CFBundleSupportedPlatforms=[iPhoneOS]`,
  `UIDeviceFamily=[1,2]`. Icons: PNG sizes written by oe from
  `project.json` `apple.icon` (asset catalogs need `actool`, a Mac tool; use
  loose `CFBundleIcons` PNGs).
- `project.json` `apple {bundleId, version, build, orientation, icon}`
  (default bundle id derived like `DefaultAndroidPackageName`).
- Signing: `--sign` runs `rcodesign sign` (`--p12-file`/`--p12-password
  env:X`, iOS `--profile x.mobileprovision` embedded as
  `embedded.mobileprovision` with entitlements taken from the profile);
  `--notarize` runs `rcodesign notary-submit --api-key-file ... --staple`.
  Unsigned output is still useful for the Simulator and for testing the
  layout. Never commit certificates, profiles or API keys.
- Ship `game/` exactly like the desktop package (project files minus
  AGENTS.md, dotfiles and `tools/`).

### Tests (`tests/tests.cpp`, run on every platform)

- `MacAppBundleLayout` / `IosIpaLayout`: package `samples/Hello` with a
  stand-in runtime file, unzip, check paths, executable bit and the parsed
  `Info.plist` keys (pure C++, runs on Windows/Linux CI too — the Android
  `AndroidApk` test is the model).
- On the macOS runner: the full `oe_tests`, plus `GpuRendererMatchesSoftware`
  on Metal when a device exists. Frame hashes come from the software renderer
  and must match the other platforms.

## Implementation status (work log)

Continue from the first unchecked item. Every checked item is committed and
pushed. Keep this list in sync with `docs/PLATFORMS.md` and `docs/ROADMAP.md`.

- [x] Plan and work log (this file)
- [ ] CI: `.github/workflows/apple.yml` builds the current `null` platform on `macos-latest` with clang and runs `oe_tests` (proves the portable code compiles on Apple clang before any new platform code). Ask the maintainer whether the repo is public (macOS minute cost)
- [ ] Shaders: add `metal_macos:metal_ios` to `compile_shaders.sh`/`.bat`, regenerate `Shaders.glsl.h`, check the Windows/web/Android builds still pass
- [ ] `engine/platform/apple/GpuMetal.mm` + `AudioApple.mm` + `CMakeLists.txt` `macos`/`ios` branches (compile-verified in CI)
- [ ] `engine/platform/macos/PlatformMacOS.mm`: game window, input, `PumpEvents`; `oe render --renderer gpu` / `GpuRendererMatchesSoftware` on the runner if Metal is available
- [ ] macOS editor event mode (+ IME, cursors, DPI, file drops), `oe_editor` with `SOKOL_METAL`; `oe editor --screenshot` on the runner
- [ ] `build_macos.sh`, prebuilt `runtime/macos/oe_player` (universal) + README, built by CI
- [ ] `oe package --macos`: `.app` + zip, `Info.plist`, icon, `project.json` `apple {...}`; `MacAppBundleLayout` test; Unix mode in `engine/core/Zip`
- [ ] macOS signing + notarization through `rcodesign` (needs a Developer ID certificate and an App Store Connect API key from the maintainer)
- [ ] `engine/platform/ios/PlatformIOS.mm`: UIKit, `CADisplayLink`, multi-touch, lifecycle, safe area, orientation; `build_ios.sh`; prebuilt `runtime/ios/` (+ simulator)
- [ ] `oe package --ios`: `.ipa`, `Info.plist`, icons; `IosIpaLayout` test; CI job runs it in the iOS Simulator and uploads a screenshot
- [ ] iOS signing with a provisioning profile (`rcodesign`); TestFlight upload from CI (`xcrun altool`)
- [ ] Hardware checks (needs a Mac and an iPhone, cannot be done in CI): Retina scaling, trackpad, IME, audio playback, real-GPU Metal, touch, notch, rotation, background/foreground
- [ ] Docs: user section at the top of this file (like `docs/ANDROID.md`), `PLATFORMS.md` table, `CLAUDE.md` + `AGENTS.md` (identical), `docs/API.md` if commands change
