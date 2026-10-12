# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android` combines it with a game's assets,
manifest and resources, so packaging needs no NDK. APK signing still needs SDK build-tools and Java; App Bundle
signing needs Java.

## Current build

- ABIs: `arm64-v8a` (phones/tablets), `x86_64` (emulators).
- Built on 2026-10-13 with Android NDK r28c (28.2.13676358), Release, API 26, static libc++, GLES3 and AAudio,
  from `main` (62d0bc5 + the image environment of docs/ENVIRONMENT.md: the `Skybox` component with three colors or an equirectangular `.hdr`/`.png` panorama, image-based ambient and reflections, the sky background and sun disc; the build before it (2026-10-12) was cc8b3a0 + `assets/game.pak` in the packed, per-game-key-enciphered `OEPAK002` format, still unpacked into the data folder, with scripts loaded as stripped Lua bytecode and the game's key patched into each `liboe_player.so` (docs/PACKAGING.md); the build before it added the navigation mesh: Recast/Detour, `nav.*`, `oe.nav`; the one before that was 0925dd0 + models with up to 1024 joints drawn in ranges of 64, `MeshRenderer.materials`, `parentPose`, blend shapes through `MeshRenderer.blendShapes`, spring bone patterns, colliders, `endLength`, `limit` and `air`, clips from other files (`<model>.clips.json`), `Animator.blend`, `BoneIK.keepRotation` / `turn`, `physics.raycast` with an entity to ignore and `CharacterBody` steps lower than the capsule radius), with the same engine features as the web runtime (see
  [../web/README.md](../web/README.md)).
- ELF load segments are aligned to 16 KB; `oe_ANativeActivity_onCreate` is exported.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 9,051,904 | a117837701fe72453cb56a65e432ff857310b7c0ba9bd82638d6dddb111558a3 |
| x86_64/liboe_player.so | 9,675,096 | 30c1e895422820780cb6e25743e3f1657e5cf86650002b50adde55923188f05a |

Packaging checks (`apksigner verify`, `aapt2 dump badging`, `zipalign -c -P 16 4`) passed for the 2026-10-10 build and were not re-run for this one. **Not run on a device**:
everything newer than the last emulator run recorded in [docs/ANDROID.md](../../docs/ANDROID.md) is unverified on
Android, including the soft keyboard, the newer shaders, networking, FBX models, models with more than 64 joints, blend shapes, bone attachments/IK/springs, physics joints, the navigation mesh, the `Skybox` environment (its half-float panorama texture and the sky pass were only run on Windows D3D11 and in a browser on WebGL2) and the `OEPAK002` game data with per-game key and stripped bytecode scripts (an APK was packaged and signed with earlier builds of this format, not installed). Earlier per-build notes are in the git
history of this file.

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
