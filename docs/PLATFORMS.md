# Platform support & porting plan

The engine is split so that only two small layers touch a platform:

1. **Platform layer** — `engine/platform/Platform.h`: window, input events, present, time, sleep, open URL, executable path, stdio mode.
2. **Render backend** — `IRenderer` in `engine/render/Renderer.h`. The software rasterizer is the reference implementation and runs everywhere (including headless CI and AI verification). Hardware backends must fill the same `RenderTarget` color/id buffers on request, so screenshots and picking behave identically on every platform.

Everything else (`core`, `scene`, `api`, `app`) is portable C++17 with no OS headers. Sockets are the one exception: `HttpServer.cpp` has Winsock and POSIX branches.

| Platform | Status | Platform layer | Renderer plan | Notes |
|---|---|---|---|---|
| Windows (x64) | **Working** | `win32/PlatformWin32.cpp` (Win32 + GDI present) | Software now; D3D12 or Vulkan next | Built with MSVC via `build.bat` |
| Headless / Linux server | **Working** (`null` platform) | `null/PlatformNull.cpp` | Software | CLI, MCP, editor server, PNG renders; no native window |
| Web (WASM) | Planned | Emscripten: canvas + `requestAnimationFrame` main loop | Software → `putImageData` first, then WebGPU | Editor already runs in a browser; the engine API would be called in-process instead of over HTTP |
| Android | Planned | NDK `android_native_app_glue`, `ANativeWindow_lock` for software present | Vulkan (GLES 3 fallback) | Touch → `InputState` keys/axes; assets from APK |
| iOS | Planned | UIKit + `CAMetalLayer` | Metal | Software frames can be shown via `CGImage` during bring-up |
| macOS | Planned | AppKit (`NSWindow` + `CAMetalLayer`) | Metal | `null` platform already builds with clang for CLI/MCP use |
| Linux desktop | Planned | X11/Wayland (or SDL3 as a shortcut) | Vulkan | |
| Nintendo Switch / PlayStation / Xbox | Planned (requires NDA SDKs) | Per-console platform file kept in a private `platforms/<name>/` folder | NVN / GNM(X) / D3D12 | See below |

## Porting checklist

1. Add `engine/platform/<name>/Platform<Name>.cpp` implementing every function in `Platform.h`.
2. Select it in `CMakeLists.txt` (`OE_PLATFORM`) using a toolchain file (Emscripten, Android NDK, Xcode, console SDK).
3. Map device input to the key names documented in `scene/Systems.h` (`W`, `Space`, `Left`, …). Add gamepad/touch axes to `InputState` when a platform needs them — the API (`input.key`) must stay able to inject the same input for automated tests.
4. Run `oe_tests` on the device or simulator. The render determinism test compares frame hashes; a hardware backend may differ from the software reference, so compare hashes per backend.

## Consoles

Console SDKs are under NDA, so their code cannot live in this public tree. The layout that keeps them out:

- `engine/platform/<console>/` and `engine/render/<api>/` in a private repository or git submodule, added through `CMakeLists.txt` only when the SDK is present.
- The engine avoids features consoles commonly restrict: no runtime code generation, no `fork`/processes, no dependency on a system shell, file access goes through `core/FileSystem`.
- The developer tooling (HTTP editor server, MCP) is for dev kits only. Shipping builds should compile it out; a `OE_ENABLE_DEVTOOLS` CMake option is the planned switch.

## Why a software renderer first

- Identical pixels on every OS and in CI, which makes frame hashes usable as test oracles for AI agents.
- No GPU or driver needed to verify a change (`oe render` works over SSH, in containers, in CI).
- Trivial to bring up on a new platform: present a CPU buffer and the platform is playable.
