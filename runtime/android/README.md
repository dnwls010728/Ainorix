# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android`
combines it with a game's assets, manifest and resources, so packaging needs
no NDK. APK signing still needs SDK build-tools and Java; App Bundle signing
needs Java.

- ABIs: `arm64-v8a` (phones/tablets), `x86_64` (emulators).
- Built on 2026-10-02 with Android NDK r28c (28.2.13676358), Clang 19.0.1,
  Release, API 26, static libc++, GLES3 and AAudio.
- Source: 81ab6ab (P6.1 camera vignette on integrated main). Both ABIs rebuilt
  with regenerated composite shaders. Includes reflected PostProcess settings,
  optional vignette and neutral defaults, alongside P1 saves, P2 animation and
  P3.1 particle simulation. Windows passes 73 tests and Node/WASM passes 69;
  D3D11/software comparison was checked. Android effect execution remains
  unverified; P3 rendering, P4 and P5 are independent feature PRs.
- ELF load segments are aligned to 16 KB; `oe_ANativeActivity_onCreate` is
  exported. Android 15 x86_64 emulator verification is recorded in
  [docs/ANDROID.md](../../docs/ANDROID.md).

## When to rebuild

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
