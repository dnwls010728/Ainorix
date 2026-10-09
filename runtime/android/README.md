# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android` combines it with a game's assets,
manifest and resources, so packaging needs no NDK. APK signing still needs SDK build-tools and Java; App Bundle
signing needs Java.

## Current build

- ABIs: `arm64-v8a` (phones/tablets), `x86_64` (emulators).
- Built on 2026-10-09 with Android NDK r28c (28.2.13676358), Release, API 26, static libc++, GLES3 and AAudio,
  from branch `claude/editor-startup`, with the same engine features as the web runtime (see
  [../web/README.md](../web/README.md)).
- ELF load segments are aligned to 16 KB; `oe_ANativeActivity_onCreate` is exported.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 7,785,232 | 3cab859ed15019faadb3075e169c42c23aa47dbb444d9f2da92b90743b46dc63 |
| x86_64/liboe_player.so | 8,256,840 | d3ee10c6f663e0e809ac69d3d2e28a9263d0561636a9ef669dff042ce630ca7b |

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
