# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android` combines it with a game's assets,
manifest and resources, so packaging needs no NDK. APK signing still needs SDK build-tools and Java; App Bundle
signing needs Java.

## Current build

- ABIs: arm64-v8a (phones/tablets), x86_64 (emulators).
- Built on 2026-10-13 with Android NDK r28c (28.2.13676358), Release, API 26 and static libc++,
  from main (975e58e), including R5 ambient-only SSAO with a depth prepass and 32 fixed samples,
  PostProcess.ssao/ssaoRadius/ssaoBias, and all earlier runtime features. See [POSTPROCESS.md](../../docs/POSTPROCESS.md).
- The engine uses GLES3 and AAudio. ELF load segments are aligned to 16 KB (llvm-readelf checked both ABIs).

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 9,215,312 | 1441c17b7f17fee8725da15ec51968d7a5499f952f4c9e5783dd1e9e523a84fa |
| x86_64/liboe_player.so | 9,857,640 | 01152e55503492e1a23604e45134e9a49f42b809a96386da29280590c9ccf76a |

**Not run on a device.** R5 was checked with 352 Windows tests, D3D11 screenshots and browser WebGL2;
the Android libraries were built and their ELF alignment verified. SSAO needs renderable R32F targets.
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
