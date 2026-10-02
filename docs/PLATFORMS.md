# Platform support & porting plan

The engine is split so that only two small layers touch a platform:

1. **Platform layer** — `engine/platform/Platform.h`: window, input events (keys, mouse/touch), present, audio output, time, sleep, open URL, executable path, stdio mode, and `CreateGpuDevice`.
2. **Render backend** — `IRenderer` in `engine/render/Renderer.h`. The software rasterizer is the reference implementation and runs everywhere (including headless CI and AI verification). `GpuRenderer` draws the same scene through sokol_gfx; a platform only supplies a `GpuDevice` (`engine/render/GpuDevice.h`: sokol environment, window swapchain, present, readback). Screenshots and picking keep using the software renderer, so they behave identically on every platform.

Everything else (`core`, `scene`, `api`, `app`, `render`) is portable C++17 with no OS headers. Sockets are the one exception: `HttpServer.cpp` has Winsock and POSIX branches.

`PlatformReplaceFile` atomically replaces a file in the same filesystem (Win32
`MoveFileExW`, POSIX `rename`). Save slots use it after closing a temporary file;
failed writes preserve the previous save. See [SAVE.md](SAVE.md).
`PlatformSaveStorage` supplies a player data directory on native platforms and
read/write localStorage callbacks on the web; engine code contains no OS calls.

| Platform | Status | Platform layer | Renderer | Notes |
|---|---|---|---|---|
| Windows (x64) | **Working** | `win32/PlatformWin32.cpp` (Win32 window, waveOut, editor event mode: text/IME, cursors, per-monitor DPI, file drops) + `win32/GpuD3D11.cpp` | Direct3D 11 (WARP fallback), software fallback | Built with MSVC via `build.bat`; `oe package` ships `Name.exe`; native editor ([EDITOR.md](EDITOR.md)) |
| Web (WASM) | **Working** | `web/PlatformWeb.cpp` (canvas, DOM input + touch, WebAudio) + `web/GpuWebGL.cpp` | WebGL2 (2D-canvas software fallback) | Prebuilt runtime in `runtime/web/` (so `oe package --web` needs no Emscripten); rebuilt with Emscripten via `build_web.sh` / `build_web.bat`. Single threaded, so any static host works (no COOP/COEP headers). Simulation is bit-identical to native |
| Headless / Linux server | **Working** (`null` platform) | `null/PlatformNull.cpp` + `null/GpuEgl.cpp` when EGL/GLES are installed | Software; GLES 3 over EGL for GPU screenshots and headless editor screenshots | CLI, MCP, HTTP API, PNG renders; no native window (no interactive editor) |
| Android | **Written, not yet device-tested** | `android/PlatformAndroid.cpp` (NativeActivity via `android_native_app_glue`, touch/mouse/keyboard/gamepad, AAudio, lifecycle, logcat) + `android/GpuAndroid.cpp` | GLES 3 through sokol_gfx (EGL window surface), software fallback into the window buffer | `liboe_player.so` built with the NDK by `build_android.sh` / `build_android.bat`; `oe package --android` writes and signs the APK (no aapt2/Gradle). See [ANDROID.md](ANDROID.md) |
| iOS | Planned | UIKit + `CAMetalLayer` | Metal through sokol_gfx (add `metal_ios` to the shader targets) | Plan + work log: [APPLE.md](APPLE.md) |
| macOS | Planned | AppKit (`NSWindow` + `CAMetalLayer`) | Metal through sokol_gfx | `null` platform already builds with clang for CLI/MCP use. The native editor runs once the window implements the event mode (below) and `sokol_imgui` is compiled with `SOKOL_METAL`. Plan + work log (developed without a Mac via CI): [APPLE.md](APPLE.md) |
| Linux desktop | Planned | X11/Wayland (or SDL3 as a shortcut) | GLES 3 / GL core through sokol_gfx | Headless GLES already works |
| Nintendo Switch / PlayStation / Xbox | Planned (requires NDA SDKs) | Per-console platform file kept in a private `platforms/<name>/` folder | Console API behind `GpuDevice`/`IRenderer` | See below |

## Porting checklist

1. Add `engine/platform/<name>/Platform<Name>.cpp` implementing every function in `Platform.h`. For GPU rendering implement `CreateGpuDevice` (a `GpuDevice` for a sokol_gfx backend, plus the `SOKOL_IMPL` translation unit — see `win32/GpuD3D11.cpp`, `web/GpuWebGL.cpp`); until then return nullptr and the software renderer is used everywhere (or compile `null/GpuNone.cpp`).
2. Select it in `CMakeLists.txt` (`OE_PLATFORM`) using a toolchain file (Emscripten, Android NDK, Xcode, console SDK).
3. Implement `CreateAudioDevice` (e.g. Web Audio, AAudio, CoreAudio); the mixer already produces 48 kHz stereo floats. Map touches to `mouseX/mouseY` + `MouseLeft`.
4. Map device input to the key names documented in `scene/Systems.h` (`W`, `Space`, `Left`, Gamepad buttons, …). Gamepad raw axes use `InputState::SetAxis` (positive Y up); `InputState::Axis` applies the shared dead zone. API `input.key` and `input.axis` inject the same logical input for automated tests. See [INPUT.md](INPUT.md) for ranges and platform adapter status.
5. Desktop platforms that should run the native editor (`engine/editor`): implement the tool side of `Window` — `SetEventMode` / `TakeEvents` (every key as `WindowKey`, UTF-32 text including IME results, all mouse buttons in client pixels, wheel, focus, close request, dropped files), `SetCursor`, `DpiScale`, `Maximize` — and `PlatformEnableHighDpi`. Map the backend in `CMakeLists.txt` so `sokol_imgui.h` gets the matching `SOKOL_<API>` define. `win32/PlatformWin32.cpp` is the reference.
6. Run `oe_tests` on the device or simulator. Frame hashes come from the software renderer and must match every other platform; `GpuRendererMatchesSoftware` checks the GPU backend against it with a tolerance.

## Networking sockets

`Platform.h` includes `platform/Network.h`: RAII non-blocking socket creation, numeric IPv4
endpoints, stream/datagram I/O and browser binary WebSockets. Networking is initialized only by
explicit factories. `Engine` construction and simulation do not open sockets or start network workers.
Native bindings default to `127.0.0.1`; a different bind address must be explicitly provided.
Native DNS and IPv6 are not implemented in M3. Browser URL resolution is handled by WebSocket.

| Backend | Implementation | Verification |
|---|---|---|
| Win32 | `platform/net/NativeSockets.cpp`: WinSock, non-blocking I/O, zero-timeout WSAPoll for connect, NODELAY | Windows Release; localhost UDP/TCP, 64 KiB channel messages, source/truncation checks, malformed frames, queue bounds, disconnect and UDP-to-TCP fallback |
| POSIX (null/macOS/Android) | Same file: BSD sockets, fcntl, zero-timeout poll, NODELAY, SIGPIPE suppression | Implemented but not compiled/run in the current Windows environment; no installed WSL distribution or NDK |
| Web | `platform/web/NetworkWeb.cpp`: binary WebSocket client, bounded JS-to-Wasm copying, callback queues | JS bridge bounds/cleanup tested with Node mocks; no Emscripten SDK or real browser/Wasm connection validation |

`net/SocketTransports.h` keeps framing, per-peer queues and lifecycle events portable. UDP peers
must be registered explicitly; TCP accept assigns transport ids and exposes the accepted source
address for M4 session validation. Browser callbacks never modify the scene. The browser client
requires a WebSocket server endpoint; the native TCP length-framing port is not a WebSocket server.
Session handshake/cookies, game server integration and Android manifest `INTERNET` permission for
enabled network projects remain follow-ups in the networking work log. Run `node tests/network_web_test.js`
for JS bridge checks; native socket tests in `oe_tests` are skipped under Emscripten.

MSVC/Ninja configuration probes the compiler's raw `/showIncludes` prefix to avoid broken header
dependency tracking when localized compiler output is decoded using the wrong code page. This
keeps incremental builds consistent after public transport structs change.

## Consoles

Console SDKs are under NDA, so their code cannot live in this public tree. The layout that keeps them out:

- `engine/platform/<console>/` and `engine/render/<api>/` in a private repository or git submodule, added through `CMakeLists.txt` only when the SDK is present.
- The engine avoids features consoles commonly restrict: no runtime code generation, no `fork`/processes, no dependency on a system shell, file access goes through `core/FileSystem`.
- The developer tooling (HTTP editor server, MCP) is for dev kits only. Shipping builds should compile it out; a `OE_ENABLE_DEVTOOLS` CMake option is the planned switch.

## Why keep a software renderer

- Identical pixels on every OS and in CI, which makes frame hashes usable as test oracles for AI agents.
- No GPU or driver needed to verify a change (`oe render` works over SSH, in containers, in CI).
- Trivial to bring up on a new platform: present a CPU buffer and the platform is playable.
