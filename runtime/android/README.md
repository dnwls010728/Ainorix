# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android` combines it with a game's assets,
manifest and resources, so packaging needs no NDK. APK signing still needs SDK build-tools and Java; App Bundle
signing needs Java.

## Current build

- ABIs: arm64-v8a (phones/tablets), x86_64 (emulators).
- Built on 2026-10-13 with Android NDK r28c (28.2.13676358), Release, API 26 and static libc++,
  from the A3/A4/A5 working tree based on main (ba403ec; A3/A4/A5 are not committed). Shared .anim.json sheet/rectangle
  atlas clips and model .clips.json display names/loop ranges/events, animation.create/get/set, dependency
  hot reload and shared event authoring are included. A4 adds variable frame durations, cumulative event
  times and last-frame hold, plus animation.get_clip/set_clip authoring with event-anchor preservation. Decimal loop seams use float playback precision
  without changing authored marker times. Earlier A1 callbacks/replay and A2 isolated preview (IK off
  by default, attachments following) remain. The native Animation panel is a desktop authoring tool.
  Shared animation asset/playback code required all player rebuilds. A5 adds validated
  .controller.json commands and embedded oe.animator Lua evaluation with typed parameters and
  ordered transitions; models forward animation.play blend seconds. Shared C++ and the embedded
  module required rebuilt players. See [ANIMATION.md](../../docs/ANIMATION.md).
  R9 screen-space reflections and all earlier runtime features remain included.
- The engine uses GLES3 and AAudio. ELF load segments are aligned to 16 KB (llvm-readelf checked both ABIs).

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 9,526,784 | 8e868212de074225839910f9c9b8aa821f0d14a1c46961cf03508afd03802b4e |
| x86_64/liboe_player.so | 10,202,568 | 7386efd2369d289967ed10f059a72a7bbc67a8d5aed12eed06d7b5ceb29801c3 |

A5 verification: validated asset writes/read-only checks, ordered transitions and triggers, model crossfades,
sprite/model native rollback and native graph mouse/text authoring pass on Windows. The packaged A5
controller demo ran in headless Edge WebGL2 through Walk, Run, Jump and Idle for model/sheet/atlas
without script/WebGL errors; software/D3D11 mean RGB difference: 0.077/255. Both Android ABIs were rebuilt
with the embedded official Lua module and retain 16 KB LOAD alignment.

**Not run on a device.** All 385 Windows tests pass, including four A4 timing/authoring/native UI tests and four A3 shared asset tests for isolation,
validation, atlas sampling, source hot reload, decimal loop seams and native replay. English/Korean/
Japanese model/sheet/atlas panel screenshots were inspected. The 1000x600 demo's software/D3D11 mean
RGB difference is 0.066/255; Showcase retains hash 72ab07fb213fd42b. The refreshed web player ran the
packaged variable-duration model/sheet/atlas demo in headless Edge WebGL2 without script/WebGL errors. Both Android
libraries retain 16 KB ELF LOAD alignment. Earlier checks remain in Git history.
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
