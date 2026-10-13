# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android` combines it with a game's assets,
manifest and resources, so packaging needs no NDK. APK signing still needs SDK build-tools and Java; App Bundle
signing needs Java.

## Current build

- ABIs: arm64-v8a (phones/tablets), x86_64 (emulators).
- Built on 2026-10-13 with Android NDK r28c (28.2.13676358), Release, API 26 and static libc++,
  from main (35e3f58), including R3 DirectionalLight.shadowCascades, stable slices, blended transitions
  and a 2048-square depth atlas; R2 SpotLight and the shared light-data texture remain included.
  The engine uses GLES3 and AAudio; all earlier runtime features remain included.
- ELF load segments are aligned to 16 KB (llvm-readelf checked both ABIs).

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 9,168,768 | 6c96aaa9c16851a9c2e57e3234185f0bc6f110135b1dc45dab01d8ec037de1ed |
| x86_64/liboe_player.so | 9,805,144 | d9ad7c92dbe82c93eeeb8eb1c17b44807f1d34ca0f2190b428ce297e1418edc2 |

**Not run on a device.** R3 was checked on Windows D3D11 and browser WebGL2, including the four-tile
depth atlas and cascade blending. The existing skin/vertex-graph uniform path still needs validation on
minimum-capability devices. Packaging validators were not re-run for this build. Earlier device and
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
