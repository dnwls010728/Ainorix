# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android` combines it with a game's assets,
manifest and resources, so packaging needs no NDK. APK signing still needs SDK build-tools and Java; App Bundle
signing needs Java.

## Current build

Built on 2026-10-14 with Android NDK r28c (28.2.13676358), Release, API 26 and static libc++,
from main a696524 plus the 2D baseline source changes committed with these artifacts (ROADMAP T1, T4,
T5, T8, T9, T10: sprite atlases, Y sorting, tile layers, pixel-perfect cameras, colored 2D lights,
sprite draw modes). Both arm64-v8a (phones/tablets) and x86_64 (emulators) also include A1-A5 animation
features, R9 SSR and earlier engine features.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 9,769,168 | 42fc05b1d6c7466c7585957acaf3219871f3160f77d961d9b36cadb2cd505db8 |
| x86_64/liboe_player.so | 10,492,552 | 812c372f495560ceb19c70a9e5e5692db70a851d5934dee2695ceb31e36f2725 |

Both ELF libraries retain 16 KB LOAD alignment (llvm-readelf). The web player rebuilt from the same
source ran the packaged 2D demo without script/WebGL errors.

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
