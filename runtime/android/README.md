# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android` combines it with a game's assets,
manifest and resources, so packaging needs no NDK. APK signing still needs SDK build-tools and Java; App Bundle
signing needs Java.

## Current build

- ABIs: arm64-v8a (phones/tablets), x86_64 (emulators).
- Built on 2026-10-13 with Android NDK r28c (28.2.13676358), Release, API 26 and static libc++,
  from main (c059b98 + three fixes: kinematic bodies keep their sweep across queries between steps, planar
  reflections show the Skybox background, spot lights wider than 60 degrees cast shadows through six faces),
  including R9 screen-space reflections, material-aware ambient replacement,
  probe/sky fallback and all earlier runtime features. See [POSTPROCESS.md](../../docs/POSTPROCESS.md).
- The engine uses GLES3 and AAudio. ELF load segments are aligned to 16 KB (llvm-readelf checked both ABIs).

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 9,340,912 | e2dc2e971398cadd90684d589ff4b5f46f024ab4dbb49afaeb622f02ffa9f9e5 |
| x86_64/liboe_player.so | 9,994,104 | 9e5eb972f20f37ef2cb9597316bd96c36f7b753ce0ea94814e6114566bf15a5e |

**Not run on a device.** R9 was checked with 361 Windows tests, D3D11 screenshots and headless
browser WebGL2 (including SSR/SSAO/DOF/bloom transitions). The Android libraries were rebuilt and
16KB ELF LOAD alignment checked for both ABIs. SSR requires R32F depth and RGBA16F render targets;
its four simultaneous color attachments and separate five-texture pass fit GLES3 minimum limits.
The mesh fragment budget remains 208 vec4 vectors plus 16 sampler uniforms (224 conservatively).
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
