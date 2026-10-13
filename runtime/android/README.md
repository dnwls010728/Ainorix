# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android` combines it with a game's assets,
manifest and resources, so packaging needs no NDK. APK signing still needs SDK build-tools and Java; App Bundle
signing needs Java.

## Current build

- ABIs: arm64-v8a (phones/tablets), x86_64 (emulators).
- Built on 2026-10-13 with Android NDK r28c (28.2.13676358), Release, API 26 and static libc++,
  from main (2f71c34), including A1 model/sprite timeline events and sidecars, shared author/query commands,
  onAnimationEvent callbacks before physics, loop/reverse/completion and native snapshot replay support.
  See [ANIMATION.md](../../docs/ANIMATION.md). R9 screen-space reflections, material-aware ambient replacement,
  probe/sky fallback and all earlier runtime features remain included.
- The engine uses GLES3 and AAudio. ELF load segments are aligned to 16 KB (llvm-readelf checked both ABIs).

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 9,393,584 | 002129e976c00c365986900abb3c1290e9380faa8c194ff457c31cae202e0a1d |
| x86_64/liboe_player.so | 10,054,248 | 2f5450f5248efdc09e776e34823ee621c26c1abff72465dda6cff79c48b51355 |

**Not run on a device.** A1 was checked with 368 Windows tests (four timeline-event cases), software/D3D11/editor
screenshots and headless Edge WebGL2 model/sprite callbacks and HUD updates. Both libraries were rebuilt and
16 KB ELF LOAD alignment checked. Earlier R9 checks covered SSR/SSAO/DOF/bloom transitions.
SSR requires R32F depth and RGBA16F render targets;
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
