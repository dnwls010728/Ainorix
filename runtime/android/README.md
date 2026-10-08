# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android` combines it with a game's assets,
manifest and resources, so packaging needs no NDK. APK signing still needs SDK build-tools and Java; App Bundle
signing needs Java.

## Current build

- ABIs: `arm64-v8a` (phones/tablets), `x86_64` (emulators).
- Built on 2026-10-08 with Android NDK r28c (28.2.13676358), Release, API 26, static libc++, GLES3 and AAudio,
  from branch `claude/rpg-village` (after main `7cc9d8b`), with the same engine features as the web runtime (see
  [../web/README.md](../web/README.md)).
- ELF load segments are aligned to 16 KB; `oe_ANativeActivity_onCreate` is exported.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 7,696,192 | ed6f71445356ea45a4df20cf614739fe6c598c4f628de9f48d844a95b5a82d2e |
| x86_64/liboe_player.so | 8,150,104 | 909f778725cfb123d11ca05850d7fedf1b6d941c85d93cb8b82d0bcbb858045c |

Packaging checks (`apksigner verify`, `aapt2 dump badging`, `zipalign -c -P 16 4`) pass. **Not run on a device**:
everything newer than the last emulator run recorded in [docs/ANDROID.md](../../docs/ANDROID.md) is unverified on
Android, including the soft keyboard, the newer shaders and networking. Earlier per-build notes are in the git
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
