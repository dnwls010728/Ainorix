# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android`
combines it with a game's assets, manifest and resources, so packaging needs
no NDK. APK signing still needs SDK build-tools and Java; App Bundle signing
needs Java.

- ABIs: `arm64-v8a` (phones/tablets), `x86_64` (emulators).
- Built on 2026-10-02 with Android NDK r28c (28.2.13676358), Clang 19.0.1,
  Release, API 26, static libc++, GLES3 and AAudio.
- Source: 0ea84f2 (P6.3a directional FXAA and highlight bloom). Both ABIs rebuilt
  with regenerated shaders. Includes reflected PostProcess settings, optional
  vignette, HDR scene buffers, exposure, Reinhard tone mapping, two-pass bloom, directional FXAA and neutral
  defaults, alongside P1 saves, P2 animation, P3 particle billboards/sample
  effects and P4 gamepad adapters. Windows tests pass 83 cases;
  Node/WASM passes 79 cases.
  D3D11/software comparison was checked. Android effect execution remains
  unverified. P5 touch changes are web-specific and included in the web player.
- ELF load segments are aligned to 16 KB; `oe_ANativeActivity_onCreate` is
  exported. Android 15 x86_64 emulator verification is recorded in
  [docs/ANDROID.md](../../docs/ANDROID.md).

## When to rebuild

Pending after PR #22: rebuild both ABIs with portable surface shader graphs,
material binding and graph-aware shadow/selection passes. Committed players still
use `0ea84f2` and cannot render graph materials. Packaged execution on Android
hardware remains unverified; see P6.4c in
[POSTPROCESS.md](../../docs/POSTPROCESS.md).

Rebuild after changing engine C++ used by the player. The runtime is shared
by every game, so it does not need rebuilding for scene, Lua or asset edits.

```bat
set ANDROID_NDK_HOME=C:\path\to\android-ndk
build_android.bat
```

`build_android.bat` / `build_android.sh` build both ABIs by default and copy
them to `build/bin/android/` and this folder. Commit both libraries together
with the source change and update this document. Packaging prefers a local
build in `build/bin/android/` and falls back to this folder.

Physical arm64 hardware, audible output and 16 KB device execution remain
unverified; ELF/APK alignment was checked, which is not a device test.

Networking M5c/M6 source adds session protocol v3, native rollback and authoritative replication.
This committed runtime was not rebuilt (required SDK unavailable); rebuild before packaging games
using these APIs. Native Windows Release and unit tests do not verify this platform runtime.

Networking M7 source adds dedicated servers, native WebSocket hosting, loopback previews
and seeded fault controls. These committed binaries have not been refreshed: rebuild
with the relevant Emscripten/Android SDK before testing the updated player on devices.
Native Windows server/wire/editor execution is verified; Wasm, Android and POSIX
execution remains unverified in this environment. See docs/NETWORK.md.
