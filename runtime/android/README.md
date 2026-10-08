# Prebuilt Android runtime

`<abi>/liboe_player.so` is the NativeActivity game player. `oe package --android`
combines it with a game's assets, manifest and resources, so packaging needs
no NDK. APK signing still needs SDK build-tools and Java; App Bundle signing
needs Java.

- ABIs: `arm64-v8a` (phones/tablets), `x86_64` (emulators).
- Built on 2026-10-04 with Android NDK r28c (28.2.13676358), Release, API 26, static libc++,
  GLES3 and AAudio.
- Source: the Dongle Zoo branch plus the UI widget work (`UIInput` with the soft keyboard through
  `Window::SetTextInput` and `KeyCharacterMap`, `UIDropdown`, `UIJoystick`, images on every UI element,
  keyboard/gamepad focus), built from the working tree; (host bridge, `input.viewSize`) on top of the VFX toolkit, the
  Water/HD-2D work, protocol v4 networking, surface shader graphs and the P7 engine changes: this
  rebuild closes the "pending rebuild" notes below. Windows tests pass 194 cases.
- Checked: both ABIs link (the UI widget work was only compiled: the soft keyboard and typing were never
  run on Android); before that work `games/DongleZoo` was packaged as APK (`apksigner verify`: v2 signature,
  `aapt2 dump badging`, `zipalign -c -P 16 4`) and as an App Bundle with the debug key.
  **Not run on a device or emulator**: none was available on this host, so every feature newer than
  the 2026-10-02 emulator run in docs/ANDROID.md is unverified on Android.
- ELF load segments are aligned to 16 KB; `oe_ANativeActivity_onCreate` is
  exported. Android 15 x86_64 emulator verification is recorded in
  [docs/ANDROID.md](../../docs/ANDROID.md).

## When to rebuild

P7 also changes prefab entity-reference remapping and shared Engine/API code.
Rebuild both ABIs from the P7 source before relying on that behavior. Android
NDK/SDK is unavailable on the verification host; these binaries remain unchanged.

Pending after PR #22: rebuild both ABIs with portable surface shader graphs,
material binding and graph-aware shadow/selection passes. Committed players still
use `0ea84f2` and cannot render graph materials. Packaged execution on Android
hardware remains unverified; see P6.4c in
[POSTPROCESS.md](../../docs/POSTPROCESS.md).

Rebuild after changing engine C++ used by the player. The runtime is shared
by every game, so it does not need rebuilding for scene, Lua or asset edits.

```bat
set ANDROID_NDK_HOME=C:\path\to\android-ndk
build_android.bat
```

`build_android.bat` / `build_android.sh` build both ABIs by default and copy
them to `build/bin/android/` and this folder. Commit both libraries together
with the source change and update this document. Packaging prefers a local
build in `build/bin/android/` and falls back to this folder.

Physical arm64 hardware, audible output and 16 KB device execution remain
unverified; ELF/APK alignment was checked, which is not a device test.

Networking M5c/M6 source adds session protocol v3, native rollback and authoritative replication.
This committed runtime was not rebuilt (required SDK unavailable); rebuild before packaging games
using these APIs. Native Windows Release and unit tests do not verify this platform runtime.

Networking M7 source adds dedicated servers, native WebSocket hosting, loopback previews
and seeded fault controls. These committed binaries have not been refreshed: rebuild
with the relevant Emscripten/Android SDK before testing the updated player on devices.
Native Windows server/wire/editor execution is verified; Wasm, Android and POSIX
execution remains unverified in this environment. See docs/NETWORK.md.

## Lua M1a-3 rebuild (2026-10-06)

Both supported ABIs rebuilt using build_android.bat, local Android NDK r28c
(28.2.13676358), API 26, Release and static libc++, with parallelism 2.
This resolves the historical pending-runtime notes above. Embedded Scene epoch,
official Lua loader/modules and ContentHash agreement require replacing the
native player libraries; the build also includes current working-tree networking.

Source provenance: base commit `eca56058cf63b79e0535d5f16c6ea33fcb3a3148`
plus the uncommitted working tree on 2026-10-06, including Lua M1a-1/2/3 and
the pre-existing networking changes. This is not a build of the base commit alone;
no commit was created. Official primitive revision: 1; manifest fingerprint:
`9ba77240e176a039`. Source SHA256: entities.lua
`0899331c3ca85b5687e7ccb5ed2351019bd67da20ebbbee8ef09001364f586f0c`,
ui.lua `fb8745dbb35f36cc6dadddf65a5b95eb448c54c444d838df026107aa1d0c49e9`.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| arm64-v8a/liboe_player.so | 6,975,920 | b242cdf01ab491b1db9f7ca040e9df3753e713a0cfa69767d1de7b466ce88fe8 |
| x86_64/liboe_player.so | 7,332,072 | e664bb8d971ab7fa40e5405a366748e3b9c744dfd1af5150f87e4cbc6b73f7be |
| OfficialSmoke.apk (temporary package, not tracked) | 14,336,681 | Debug-signed fixture containing both ABIs |

Verified: build_android.bat exit 0; oe package --android builds/signs the isolated
OfficialSmoke project using build-tools 35.0.0 and Adoptium Java 25.0.1.
apksigner verify --verbose reports v2/v3 signatures valid; aapt2 dump badging
reports arm64-v8a/x86_64 and API 26/35; zipalign -c -P 16 4 exits 0.
No official Lua source files are placed in game.pak. Compilation and signing
do not demonstrate Lua import on Android: no device/emulator was available.
**Device execution unverified (기기 실행 미검증)**: import, UI, networking,
audio, physical arm64 and 16 KB device execution need a connected device/emulator.
Existing network-source Clang warnings and Java native-access warnings are recorded
in the build/verification logs; they did not prevent building or APK verification.

## M1b widget pilot rebuild (2026-10-06)

NDK r28c (28.2.13676358) from build/android-sdk, Release, API 26, static libc++, parallelism 2.
Base commit eca56058cf63b79e0535d5f16c6ea33fcb3a3148 plus the uncommitted
working tree, including prior M1a/networking changes and M1b widgets; no new commit.
Primitive revision 1, official digest 58adf79c7598d716.
oe/ui.lua SHA256 20c1f0b93dccb91e22f3497bcb5734d713da0672da6504ee662ed34a70b65684.
The UI source is embedded: packaging Lua game data alone cannot update it.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/android/arm64-v8a/liboe_player.so | 6,978,464 | dc3277ee4f5541d93f24734c07137d435468ec75b3bc6ae02151901f6e0166fc |
| runtime/android/x86_64/liboe_player.so | 7,334,616 | 23fb8e1a398cecc85240a8e7c91e9d01bad037450d3decc2ed1eba1ddb2f134c |

The final player rebuild ran once after official sources were finalized, exit 0.
Desktop 223 tests and actual-process OnlineRPG end-to-end pass; five desktop
pilot images have identical before/after hashes/PNG bytes. This runtime contains
capability-aware widgets and the new official source agreement fingerprint.
Both supported ABI libraries changed. APK/device/emulator execution was not repeated
in M1b. Device execution remains unverified (기기 실행 미검증); requires a device/emulator.
The prior M1a-3 APK signing/alignment results above are historical, not new-device evidence.

## M2a coordinate query rebuild (2026-10-06)

NDK r28c (28.2.13676358), API 26, Release, static libc++, parallelism 2.
Base checkout commit da530f75508eac9f2ec63266aba94aa9761925f2 plus current uncommitted source, including
pre-existing networking/M1a/M1b/editor work. No commit was created.
The final android player rebuild ran once after M2a engine/official sources were finalized,
exit 0. Camera queries use shared renderer matrices and the existing canvas scale.
New native primitives are additive: existing primitive contracts and revision 1 remain
unchanged; the new oe.ui.screenToCanvas forwarding changes official digest
58adf79c7598d716 -> 2b06cf1f108cb7cf. Matching players must be deployed together.
Source SHA256: CoordinateQueries.cpp 49c5854a7e1aa38e1f72ba772747909282027e3f70306e79cb5fdd9ca0555f29;
oe/ui.lua 4abc66f722a27f1aedd900a0add0bf604cafe397b62a7a76cf11a6aa6bf34293.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/android/arm64-v8a/liboe_player.so | 7,004,128 | 4c4d00af19dfa688748304721ee6c8f896de93c7e0686b0d7b5c57535f481afa |
| runtime/android/x86_64/liboe_player.so | 7,364,584 | 254e837b82e7f433ec04b9036ca5287c5d75ee114970baa1199126c198169952 |

Both supported ABI libraries were rebuilt/copied and changed. No APK packaging/signing or
device/emulator execution was performed in M2a. **Device execution unverified (기기 실행
미검증)**: a connected device/emulator is required for pointer/UI/network and 16 KB execution.
Prior package/alignment results above remain historical evidence.
Desktop 226 tests and actual-process OnlineRPG end-to-end pass; all eight software pilot
screenshots preserve hashes and PNG bytes. Source/projection tests and measurement details:
docs/LUA_USABILITY.md 6.4/8. Existing network aggregate-initializer warnings remain.

## M2b text measurement rebuild (2026-10-06)

NDK r28c, API 26, Release, static libc++, both supported ABIs, parallelism 2.
Base checkout da530f75508eac9f2ec63266aba94aa9761925f2 plus the existing uncommitted working tree and
M2b changes. No commit was created. Final player rebuild ran once after engine/official
source finalization, exit 0. Additive native ui.measureText shares actual renderer shaping;
existing primitive behavior/revision 1 remain unchanged. Official digest
2b06cf1f108cb7cf -> dff0818781f4b2b4 (oe.ui forwarding); matching players required.
Source SHA256: UI.cpp 85b1c3be6eab9a4cb074a4ba51a6be853355af7f11a177ce029af42174909bf1;
CoordinateQueries.cpp 54358fc2e031c8d592280f7509ccbc9ce43bc0194b813485154cdcfa7186d33b;
oe/ui.lua 864206a35a6cb1cce51c88f014815e3c1917c0907b4b88ffb30ca27d97c043b0.
Embedding normalizes UTF-8 line endings; source hashes above identify working-tree bytes.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/android/arm64-v8a/liboe_player.so | 7,011,840 | a7209e29a842f850120cde0f7cafd80f50b853f4402bba1025e2505fb50bd14e |
| runtime/android/x86_64/liboe_player.so | 7,373,752 | a1bc26d98c31f7b18fd1ad00828f3e141a85248ba851d644e4224bfa9cb4bf28 |

Both ABI libraries changed and were refreshed. No M2b APK packaging/signing or
device/emulator execution was performed. **Device execution unverified (기기 실행 미검증)**:
a device/emulator is required for text/font/UI/network and physical 16 KB checks. Prior
APK evidence above is historical. Compilation does not prove Android execution.
Desktop 228 tests and OnlineRPG actual-process end-to-end pass. Six new multilingual
images change intentionally with measured pill widths/bubble heights; unchanged UI controls
preserve prior hashes. Existing network initializer and Wasm unused-helper warnings remain;
a new test range-copy warning was fixed before final suite rebuild. Shared fixture folders
require desktop/Wasm suites to run sequentially; interrupted/concurrent attempts are recorded
in docs/LUA_USABILITY.md 6.5/8, not counted as successful validation.

## M3 input/math runtime rebuild (2026-10-06)

Base checkout da530f75508eac9f2ec63266aba94aa9761925f2 plus the existing uncommitted
working tree and M3 changes; no commit. New embedded oe.input/oe.math sources and
manifest are shared by every player; C++ primitives and protocol v4 are unchanged.
Official manifest digest dff0818781f4b2b4 -> 403839f0fff280c5, primitive revision 1.
Digest computed using OfficialManifestDigest canonicalization and core/Image.cpp:240
seed; UTF-8 LF normalization. Source SHA256 (working-tree bytes):
input.lua 6b98002a2beb53661de61d60ea30ce24ffe65ebe607210696b65dbadeaaedf1b;
math.lua 65823eb38d15810a1c99f812c6d3f276b287eac876dbbea93bf7ba979ed71d50.

Final builds and tests exited 0. Desktop 230 tests; sequential Node/Wasm 194 tests.
Initial runtime rebuilds were superseded by the final reviewed-source rebuilds:
review found input.player's lobby fallback to device input, so playerSource now
requires a running match; math rejects false-valued coordinates too. Two runtime
build passes occurred, contrary to the requested single final pass; final bytes
below contain these corrections. Existing Wasm unused ProcessFixtureDir warning
remains; no new module warning was observed. Details/logs: build/m3/.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/android/arm64-v8a/liboe_player.so | 7,016,672 | b96bdf2d2f310bd658f7601774b41df891eb39eeb76827090410e57c83a299f6 |
| runtime/android/x86_64/liboe_player.so | 7,378,648 | 1a2375aa282d1cc14991024498d218695901c4bb45b6a78caa4e433fa6ce5a14 |

ANDROID_HOME=build/android-sdk, NDK r28c, API 26, Release, static libc++, parallelism 2.
Both supported ABIs rebuilt and refreshed. No APK/signing run or connected device/
emulator execution in M3. Device execution, physical touch/UIJoystick and gamepad
remain unverified (기기 실행 미검증); a connected device/controller is required.

## Required-only Lua module reload runtime refresh (2026-10-07)

Source: HEAD da530f75508eac9f2ec63266aba94aa9761925f2 plus the preserved dirty working
tree and LUA_USABILITY.md 6.10. No commit. Players contain dependency-aware transactional
project reload and the ordinary Lua snapshot allocator. The final pass adds same-table
forwarding safety and prevents project aliases from mutating cached official exports.
Initial gameplay, protocol v4 and official manifest digest 403839f0fff280c5 / primitive
revision 1 remain unchanged; all official Lua sources unchanged.

Native final build/full 235 tests and Node/Wasm full 199 tests exit 0. Native network,
snapshot, GPU/software and both suites' reload regressions pass. Two player build passes
were required because review found a shared-export alias bug after the first pass;
this deviates from the planned single final runtime pass. These hashes are corrected bytes.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/android/arm64-v8a/liboe_player.so | 7,051,888 | 5d3f900b32fa6f32c4cc5f9eca831a46d08158336e460bda330ff9dc5fabf442 |
| runtime/android/x86_64/liboe_player.so | 7,415,128 | 60dc6a7a11c1a11b421dc13f9d29db9bd86726e533bcdc1d80f21ae6669ce859 |

Engine reload source SHA256 (working-tree bytes):

engine/script/ScriptHost.h: `ee08ab68c6d68f428d97fab48ba95b14a26cb7fe03961c7ef9e30fe23264d9e7`
engine/script/ScriptHost.cpp: `03729acf2e30d0bcb7120297487a074e04f9e9e031675e4edb1736161e9eb8eb`
engine/app/Engine.h: `41a400e0f5e1a70821ee6f025374774f61445ddfdf9c8dd44f93bdd20c7a21b2`
engine/app/Engine.cpp: `5bdb778f086a514eaf94ad86fa4b2852435a0ec82041d43aab3eaf7a3c109d0a`
engine/api/Commands.cpp: `b325687e72073839315c80ec5c117362d659ae884cb4ca108197f1be4ef4adc0`

build_android.bat, both supported ABIs, ANDROID_HOME=build/android-sdk, NDK r28c,
API 26, Release, static libc++, parallelism 2. arm64 grew by 35,216 bytes; x86_64 by
36,480 bytes for loader/transaction/alias safety. Initial full rebuild emitted existing
unrelated network/codec missing-field initializer warnings; final incremental rebuild
had no reload-source warning. No new APK signing or device/emulator launch in this unit:
기기 실행 미검증. ADB was not found in this SDK or on PATH.
Final evidence: ignored build/reload/*release* and *reviewed-build* logs, artifacts.json.

## Orthographic Scene-view support runtime refresh (2026-10-07)

Source: main HEAD a822d27 plus the working-tree editor window-placement,
independent 2D/3D view persistence and orthographic free-camera changes.
The player-shared command parser now accepts `camera.projection: "orthographic"`
and `orthoSize` for matching screenshots and picking; default perspective and
scene-camera rendering are unchanged. Official Lua sources/digest are unchanged.
Commands.cpp SHA256: `eacd7f7bab83df0c74963fa6ef8fcf1d571b4a02bb63ca27fca91de362231df4`.
Native build and full suite: 245 tests, 0 failed checks, exit 0. The editor 2D
screenshot was visually inspected. No new packaged browser or Android device
execution was performed; Wasm tests were not rerun in this unit.
| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/android/arm64-v8a/liboe_player.so | 7,053,744 | cc87bb669326f93f763fed5964f5caf53fa8cfaf1c940b487923708a65584717 |
| runtime/android/x86_64/liboe_player.so | 7,417,096 | 561fae039ecc1d8f6e7d0689685b78ba1f74a2b46538b1453566c9dabc32cacb |

build_android.bat with ANDROID_HOME=build/android-sdk succeeded for both supported
ABIs. arm64 grew by 1,856 bytes and x86_64 by 1,968 bytes for the shared command
parser and rebuilt platform interface. Device/emulator execution remains unverified.
Evidence: ignored build/orthographic-android-build.log.

## VFX phase 4 rebuild (2026-10-08)

The player includes VFXSystem assets, materials, normalized age/random/speed graph inputs, optional
emitter batching, bounded death chains, two-anchor ribbons and static mesh particles. Unbatched
particles keep shared GPU meshes and use a dedicated draw input; batching uses vertex RGBA/data.
Legacy particle hashes remain unchanged. See docs/VFX_PHASE4.md for validation and known limits.

Built with Android NDK r28c, Release, for arm64-v8a and x86_64. Native Windows/D3D11 tests
pass; Android device/emulator execution of these effects remains unverified.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/android/arm64-v8a/liboe_player.so | 7,409,776 | a0bf24b6ac836be34c91bc7bfeaa2f75b6daae8fe6b73ed557652fcf7faae66d |
| runtime/android/x86_64/liboe_player.so | 7,828,920 | 56d02ef6d45bfffa2d7dc80bf3d66be28a72709271021aaa491b6db54ffa7d83 |

Commands.cpp SHA256: `9e32bd478583e35831c86902197af9d2458133654dae8ba999c392db624fed74`.

## VFX motion channels rebuild (phase 5a, 2026-10-08)

Players include captured birth state, motion/XYZ scale curves, pivots and four custom material
attributes in both rendering paths and batching. See docs/VFX_PHASE5A.md for the contract and validation.
Built with Android NDK r28c, Release, parallelism 2, for arm64-v8a and x86_64.
Device/browser GPU execution remains unverified.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/android/arm64-v8a/liboe_player.so | 7,458,096 | 338dfb0b4ea84f5ca6aaeea12e8fd238496541b2c19c317ef3e6cf8a759803d2 |
| runtime/android/x86_64/liboe_player.so | 7,885,816 | 522953612cb25ce4d3d188a3edbdb4b8daca34809fa04572ae1b32a558154de1 |

## VFX expressions rebuild (phase 5b, 2026-10-08)

Players include typed Birth/Update expressions, own bounded math, cached programs, parameter inputs
and conservative instruction budgets. Contract: docs/VFX_PHASE5B.md. Windows/Node assert the same
math and software-preview hashes; all 239 Node tests pass. Device/browser GPU execution remains
unverified. Toolchains: Emscripten 6.0.10 / Android NDK r28c, Release, parallelism 2.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/android/arm64-v8a/liboe_player.so | 7,539,280 | eec339f5c070589867b768faf2acbb90eabbbfba6f2cee070256686141934d58 |
| runtime/android/x86_64/liboe_player.so | 7,976,856 | 46f6d3fbf9f26fce2f41c6cadd2b60e076076756845f829c1f452cf352873528 |

Commands.cpp SHA256: `e9777ebc426db0bf619d384f6c28522ba625809c22eab950e52bc9aa6e62fa29`.
