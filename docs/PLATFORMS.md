# Platform support & porting plan

The engine is split so that only two small layers touch a platform:

1. **Platform layer** — `engine/platform/Platform.h`: window, input events (keys, mouse/touch), present, audio output, time, sleep, open URL, executable path, stdio mode, and `CreateGpuDevice`.
2. **Render backend** — `IRenderer` in `engine/render/Renderer.h`. The software rasterizer is the reference implementation and runs everywhere (including headless CI and AI verification). `GpuRenderer` draws the same scene through sokol_gfx; a platform only supplies a `GpuDevice` (`engine/render/GpuDevice.h`: sokol environment, window swapchain, present, readback). Screenshots and picking keep using the software renderer, so they behave identically on every platform.

Everything else (`core`, `scene`, `api`, `app`, `render`) is portable C++17 with no OS headers. Sockets are the one exception: `HttpServer.cpp` has Winsock and POSIX branches.

| Platform | Status | Platform layer | Renderer | Notes |
|---|---|---|---|---|
| Windows (x64) | **Working** | `win32/PlatformWin32.cpp` (Win32 window, waveOut, editor event mode: text/IME, cursors, per-monitor DPI, file drops) + `win32/GpuD3D11.cpp` | Direct3D 11 (WARP fallback), software fallback | Built with MSVC via `build.bat`; `oe package` ships `Name.exe`; native editor ([EDITOR.md](EDITOR.md)) |
| Web (WASM) | **Working** | `web/PlatformWeb.cpp` (canvas, DOM input + touch, WebAudio) + `web/GpuWebGL.cpp` | WebGL2 (2D-canvas software fallback) | Prebuilt runtime in `runtime/web/` (so `oe package --web` needs no Emscripten); rebuilt with Emscripten via `build_web.sh` / `build_web.bat`. Single threaded, so any static host works (no COOP/COEP headers). Simulation is bit-identical to native |
| Headless / Linux server | **Working** (`null` platform) | `null/PlatformNull.cpp` + `null/GpuEgl.cpp` when EGL/GLES are installed | Software; GLES 3 over EGL for GPU screenshots and headless editor screenshots | CLI, MCP, HTTP API, PNG renders; no native window (no interactive editor) |
| Android | Planned | NDK `android_native_app_glue` | GLES 3 through sokol_gfx (the WebGL2 shaders already compile for it) | Touch → `InputState` keys/axes; assets from APK |
| iOS | Planned | UIKit + `CAMetalLayer` | Metal through sokol_gfx (add `metal_ios` to the shader targets) | |
| macOS | Planned | AppKit (`NSWindow` + `CAMetalLayer`) | Metal through sokol_gfx | `null` platform already builds with clang for CLI/MCP use. The native editor runs once the window implements the event mode (below) and `sokol_imgui` is compiled with `SOKOL_METAL` |
| Linux desktop | Planned | X11/Wayland (or SDL3 as a shortcut) | GLES 3 / GL core through sokol_gfx | Headless GLES already works |
| Nintendo Switch / PlayStation / Xbox | Planned (requires NDA SDKs) | Per-console platform file kept in a private `platforms/<name>/` folder | Console API behind `GpuDevice`/`IRenderer` | See below |

## Porting checklist

1. Add `engine/platform/<name>/Platform<Name>.cpp` implementing every function in `Platform.h`. For GPU rendering implement `CreateGpuDevice` (a `GpuDevice` for a sokol_gfx backend, plus the `SOKOL_IMPL` translation unit — see `win32/GpuD3D11.cpp`, `web/GpuWebGL.cpp`); until then return nullptr and the software renderer is used everywhere (or compile `null/GpuNone.cpp`).
2. Select it in `CMakeLists.txt` (`OE_PLATFORM`) using a toolchain file (Emscripten, Android NDK, Xcode, console SDK).
3. Implement `CreateAudioDevice` (e.g. Web Audio, AAudio, CoreAudio); the mixer already produces 48 kHz stereo floats. Map touches to `mouseX/mouseY` + `MouseLeft`.
4. Map device input to the key names documented in `scene/Systems.h` (`W`, `Space`, `Left`, …). Add gamepad/touch axes to `InputState` when a platform needs them — the API (`input.key`) must stay able to inject the same input for automated tests.
5. Desktop platforms that should run the native editor (`engine/editor`): implement the tool side of `Window` — `SetEventMode` / `TakeEvents` (every key as `WindowKey`, UTF-32 text including IME results, all mouse buttons in client pixels, wheel, focus, close request, dropped files), `SetCursor`, `DpiScale`, `Maximize` — and `PlatformEnableHighDpi`. Map the backend in `CMakeLists.txt` so `sokol_imgui.h` gets the matching `SOKOL_<API>` define. `win32/PlatformWin32.cpp` is the reference.
6. Run `oe_tests` on the device or simulator. Frame hashes come from the software renderer and must match every other platform; `GpuRendererMatchesSoftware` checks the GPU backend against it with a tolerance.

## Consoles

Console SDKs are under NDA, so their code cannot live in this public tree. The layout that keeps them out:

- `engine/platform/<console>/` and `engine/render/<api>/` in a private repository or git submodule, added through `CMakeLists.txt` only when the SDK is present.
- The engine avoids features consoles commonly restrict: no runtime code generation, no `fork`/processes, no dependency on a system shell, file access goes through `core/FileSystem`.
- The developer tooling (HTTP editor server, MCP) is for dev kits only. Shipping builds should compile it out; a `OE_ENABLE_DEVTOOLS` CMake option is the planned switch.

## Why keep a software renderer

- Identical pixels on every OS and in CI, which makes frame hashes usable as test oracles for AI agents.
- No GPU or driver needed to verify a change (`oe render` works over SSH, in containers, in CI).
- Trivial to bring up on a new platform: present a CPU buffer and the platform is playable.
