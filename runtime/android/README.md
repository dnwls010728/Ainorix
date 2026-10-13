# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android` combines it with a game's assets,
manifest and resources, so packaging needs no NDK. APK signing still needs SDK build-tools and Java; App Bundle
signing needs Java.

## Current build

- ABIs: arm64-v8a (phones/tablets), x86_64 (emulators).
- Built on 2026-10-13 with Android NDK r28c (28.2.13676358), Release, API 26 and static libc++,
  from main (b745881), including R4 point/spot shadows, two-light selection, twelve perspective maps
  in a 2048-square atlas and the 8x18 light-data texture. All earlier runtime features remain included.
- The engine uses GLES3 and AAudio. ELF load segments are aligned to 16 KB (llvm-readelf checked both ABIs).

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 9,198,288 | db9f91a648f3cac661ef20cb66530ee8e2e11b830f3a30e668d6b833f808618c |
| x86_64/liboe_player.so | 9,839,720 | cfe3eaa7f2c4ea9163c12e847690fe0a89d16ebed8840640ba4dbb4dcbd3e6fa |

**Not run on a device.** R4 was checked on Windows D3D11 and browser WebGL2; the Android libraries were
built and their ELF alignment verified. The existing skin/vertex-graph uniform path still needs validation
on minimum-capability devices. Packaging validators were not re-run for this build. Earlier device and
packaging checks, and remaining newer engine-feature checks, are recorded in
[ANDROID.md](../../docs/ANDROID.md). Earlier runtime build history is preserved in Git.

## When to rebuild

Rebuild after changing engine C++ used by the player. The runtime is shared by every game, so it does not need
rebuilding for scene, Lua or asset edits.

```bat
set ANDROID_NDK_HOME=C:\path\to\android-ndk
build_android.bat
```

`build_android.bat` / `build_android.sh` build both ABIs by default and copy them to `build/bin/android/` and this
folder. Commit both libraries together with the source change and update the table above. Packaging prefers a
local build in `build/bin/android/` and falls back to this folder.
