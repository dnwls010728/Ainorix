# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android`
combines it with a game's assets, manifest and resources, so packaging needs
no NDK. APK signing still needs SDK build-tools and Java; App Bundle signing
needs Java.

- ABIs: `arm64-v8a` (phones/tablets), `x86_64` (emulators).
- Built on 2026-10-04 with Android NDK r28c (28.2.13676358), Release, API 26, static libc++,
  GLES3 and AAudio.
- Source: the Dongle Zoo branch (host bridge, `input.viewSize`) on top of the VFX toolkit, the
  Water/HD-2D work, protocol v4 networking, surface shader graphs and the P7 engine changes: this
  rebuild closes the "pending rebuild" notes below. Windows tests pass 189 cases.
- Checked: both ABIs link; `games/DongleZoo` packaged as APK (`apksigner verify`: v2 signature,
  `aapt2 dump badging`, `zipalign -c -P 16 4`) and as an App Bundle with the debug key.
  **Not run on a device or emulator**: none was available on this host, so every feature newer than
  the 2026-10-02 emulator run in docs/ANDROID.md is unverified on Android.
- ELF load segments are aligned to 16 KB; `oe_ANativeActivity_onCreate` is
  exported. Android 15 x86_64 emulator verification is recorded in
  [docs/ANDROID.md](../../docs/ANDROID.md).

## When to rebuild

P7 also changes prefab entity-reference remapping and shared Engine/API code.
Rebuild both ABIs from the P7 source before relying on that behavior. Android
NDK/SDK is unavailable on the verification host; these binaries remain unchanged.

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
