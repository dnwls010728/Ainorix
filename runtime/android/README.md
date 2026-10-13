# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android` combines it with a game's assets,
manifest and resources, so packaging needs no NDK. APK signing still needs SDK build-tools and Java; App Bundle
signing needs Java.

## Current build

- ABIs: arm64-v8a (phones/tablets), x86_64 (emulators).
- Built on 2026-10-13 with Android NDK r28c (28.2.13676358), Release, API 26 and static libc++,
  from main (ebabcd7), including R7 Decal box projection, order/fades, the shared atlas and 8x33
  light-data texture, and all earlier runtime features. See [DECALS.md](../../docs/DECALS.md).
- The engine uses GLES3 and AAudio. ELF load segments are aligned to 16 KB (llvm-readelf checked both ABIs).

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 9,290,784 | dd2a97cb13d0549b7b7aae9db0166f0ea359cf4b46717f4b0afd6f0c92439222 |
| x86_64/liboe_player.so | 9,939,000 | 61a51b2e4ee357f2ca01d3509a216bf50b4129dc6ae48cbbe82d4904db832261 |

**Not run on a device.** R7 was checked with 357 Windows tests, D3D11 screenshots and browser WebGL2;
the Android libraries were built and their ELF alignment verified. Decals use an RGBA8 atlas; HDR/SSAO/DOF retain their separate format requirements.
The existing skin/vertex-graph uniform path still needs validation
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
