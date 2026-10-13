# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android` combines it with a game's assets,
manifest and resources, so packaging needs no NDK. APK signing still needs SDK build-tools and Java; App Bundle
signing needs Java.

## Current build

Built on 2026-10-14 with Android NDK r28c (28.2.13676358), Release, API 26 and static libc++,
from main 8511b48 plus the live-controller observation source change committed with these artifacts.
Both arm64-v8a (phones/tablets) and x86_64 (emulators) include A1-A5 animation features and the
shared oe.animator weak owner registry, 16-transition trace and animation.controller_state bridge.
Controller evaluation, public state() results and controller formats remain unchanged; native
snapshots restore observation state. Shared C++ and embedded Lua changes require rebuilt players.
The graph visualization itself is editor-only. R9 SSR and earlier engine features remain included.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 9,536,960 | e031b88105d0aa56bd94e681165bf166862b8d4d0bc4ba3529788a791b5eb990 |
| x86_64/liboe_player.so | 10,213,432 | 66ee794a0b0959447932301f0617805d2c2d537f5ffab791557d75bf4f813463 |

Both ELF libraries retain 16 KB LOAD alignment (llvm-readelf). The freshly rebuilt web player ran
the packaged model/sheet/atlas controller demo through Walk, Run, Jump and Idle without script/WebGL
errors. Windows tests cover observation isolation, weak lifecycle, native rollback and graph flow.

**Not run on a device.** APK/AAB signing/install validation was not repeated for this runtime-only
refresh. Physical-device performance, input/audio and minimum-capability GPU paths remain unverified;
see [ANDROID.md](../../docs/ANDROID.md). Earlier runtime build history is preserved in Git.

## When to rebuild

Rebuild after changing engine C++ or embedded official Lua modules used by the player. The runtime is shared by every game, so it does not need
rebuilding for project scene, Lua or asset edits.

```bat
set ANDROID_NDK_HOME=C:\path\to\android-ndk
build_android.bat
```

`build_android.bat` / `build_android.sh` build both ABIs by default and copy them to `build/bin/android/` and this
folder. Commit both libraries together with the source change and update the table above. Packaging prefers a
local build in `build/bin/android/` and falls back to this folder.
