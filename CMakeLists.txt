cmake_minimum_required(VERSION 3.20)
project(OwnEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Static C/C++ runtime: packaged games run on PCs without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Ninja must match the compiler's actual /showIncludes bytes. Localized MSVC output
# can be incorrectly decoded during CMake's compiler detection, silently losing
# header dependencies and mixing incompatible objects after a public struct changes.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  set(OE_INCLUDES_PROBE ${CMAKE_BINARY_DIR}/generated/OwnEngineShowIncludes.cpp)
  file(WRITE ${CMAKE_BINARY_DIR}/generated/OwnEngineShowIncludes.h "#pragma once\n")
  file(WRITE ${OE_INCLUDES_PROBE} "#include \"OwnEngineShowIncludes.h\"\n")
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} /nologo /showIncludes /Zs ${OE_INCLUDES_PROBE}
    OUTPUT_VARIABLE OE_INCLUDES_OUTPUT ERROR_VARIABLE OE_INCLUDES_ERROR
    RESULT_VARIABLE OE_INCLUDES_RESULT ENCODING NONE TIMEOUT 10)
  if(OE_INCLUDES_RESULT EQUAL 0)
    string(REGEX MATCH "[^\r\n]*OwnEngineShowIncludes\\.h" OE_INCLUDES_LINE "${OE_INCLUDES_OUTPUT}${OE_INCLUDES_ERROR}")
    if(OE_INCLUDES_LINE MATCHES "[A-Za-z]:[/\\\\]")
      string(REGEX REPLACE "[A-Za-z]:[/\\\\].*$" "" CMAKE_CL_SHOWINCLUDES_PREFIX "${OE_INCLUDES_LINE}")
    endif()
  endif()
endif()

option(OE_BUILD_TESTS "Build the engine test executable" ON)

# ---------------------------------------------------------------------------
# Platform selection. Every platform implements engine/platform/Platform.h.
# Win32 (desktop), Web (Emscripten, see build_web.sh), Android (NDK, see
# build_android.sh) and Null (headless); the others are planned and listed
# in docs/PLATFORMS.md.
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
  set(OE_PLATFORM "web")
elseif(ANDROID)
  set(OE_PLATFORM "android")
elseif(WIN32)
  set(OE_PLATFORM "win32")
else()
  set(OE_PLATFORM "null")
endif()
message(STATUS "OwnEngine platform backend: ${OE_PLATFORM}")

# ---------------------------------------------------------------------------
# Lua 5.4 (vendored, unmodified). Compiled as C++ so Lua errors unwind C++
# destructors; lstate.c is built through a wrapper that fixes the hash seed
# for deterministic simulation.
# ---------------------------------------------------------------------------
file(GLOB OE_LUA_SOURCES third_party/lua/*.c)
list(FILTER OE_LUA_SOURCES EXCLUDE REGEX ".*/lstate\.c$")
list(APPEND OE_LUA_SOURCES third_party/lua_oe/lstate_deterministic.c)
set_source_files_properties(${OE_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(oe_lua STATIC ${OE_LUA_SOURCES})
target_include_directories(oe_lua PUBLIC third_party/lua)
if(MSVC)
  target_compile_options(oe_lua PRIVATE /W0 /TP)
  target_compile_definitions(oe_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(oe_lua PRIVATE -w -x c++)
endif()
if(ANDROID)
  # ARM compilers fuse a*b+c into FMA by default, which changes results:
  # keep the simulation bit-identical to the other platforms.
  target_compile_options(oe_lua PRIVATE -ffp-contract=off)
endif()

# ---------------------------------------------------------------------------
# Jolt Physics (vendored, unmodified). Cross-platform deterministic mode so a
# simulation gives identical results on every OS/CPU; SSE4.2 baseline for
# wide CPU support; GPU compute, debug renderer and profiler are not needed.
# ---------------------------------------------------------------------------
set(CROSS_PLATFORM_DETERMINISTIC ON)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY ON)
set(INTERPROCEDURAL_OPTIMIZATION OFF)
set(OVERRIDE_CXX_FLAGS OFF)
set(GENERATE_DEBUG_SYMBOLS OFF)
set(ENABLE_ALL_WARNINGS OFF)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF)
set(ENABLE_OBJECT_STREAM OFF)
set(ENABLE_INSTALL OFF)
set(USE_AVX OFF)
set(USE_AVX2 OFF)
set(USE_LZCNT OFF)
set(USE_TZCNT OFF)
set(USE_F16C OFF)
set(USE_FMADD OFF)
set(JPH_USE_DX12 OFF)
set(JPH_USE_VK OFF)
set(JPH_USE_MTL OFF)
set(JPH_USE_CPU_COMPUTE OFF)
add_subdirectory(third_party/jolt/Build ${CMAKE_BINARY_DIR}/jolt EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Box2D 3.1 (vendored, unmodified): the 2D physics backend (Collider2D,
# RigidBody2D, CharacterBody2D). C17; no FMA contraction so results are the
# same on every compiler/CPU (Box2D is cross-platform deterministic).
# ---------------------------------------------------------------------------
file(GLOB OE_BOX2D_SOURCES third_party/box2d/src/*.c)
add_library(oe_box2d STATIC ${OE_BOX2D_SOURCES})
target_include_directories(oe_box2d PUBLIC third_party/box2d/include)
set_target_properties(oe_box2d PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS YES)
if(MSVC)
  target_compile_options(oe_box2d PRIVATE /W0)
  target_compile_definitions(oe_box2d PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(oe_box2d PRIVATE -w -ffp-contract=off)
endif()
if(EMSCRIPTEN)
  target_compile_definitions(oe_box2d PRIVATE BOX2D_DISABLE_SIMD)  # scalar path: no -msimd128 needed
endif()

file(GLOB OE_CORE_SOURCES CONFIGURE_DEPENDS
  engine/core/*.cpp
  engine/scene/*.cpp
  engine/script/*.cpp
  engine/physics/*.cpp
  engine/audio/*.cpp
  engine/net/*.cpp
  engine/net/sync/*.cpp
  engine/assets/*.cpp
  engine/render/*.cpp
  engine/api/*.cpp
  engine/app/*.cpp)

# Built-in UI font (third_party/fonts), embedded as a byte array so every
# build - desktop, web, headless - has a real TrueType font without files.
set(OE_FONT_FILE ${CMAKE_SOURCE_DIR}/third_party/fonts/Roboto-Regular.ttf)
set(OE_FONT_CPP ${CMAKE_BINARY_DIR}/generated/EmbeddedFonts.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OE_FONT_FILE})
if(NOT EXISTS ${OE_FONT_CPP} OR ${OE_FONT_FILE} IS_NEWER_THAN ${OE_FONT_CPP})
  file(READ ${OE_FONT_FILE} OE_FONT_HEX HEX)
  string(REPEAT "[0-9a-f]" 64 OE_HEX_LINE)
  string(REGEX REPLACE "(${OE_HEX_LINE})" "\\1\n" OE_FONT_HEX "${OE_FONT_HEX}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," OE_FONT_HEX "${OE_FONT_HEX}")
  file(WRITE ${OE_FONT_CPP} "// Generated by CMakeLists.txt from third_party/fonts/Roboto-Regular.ttf. Do not edit.\n#include <cstddef>\nnamespace oe {\nextern const unsigned char kDefaultFontData[] = {\n${OE_FONT_HEX}};\nextern const size_t kDefaultFontSize = sizeof(kDefaultFontData);\n}  // namespace oe\n")
endif()
list(APPEND OE_CORE_SOURCES ${OE_FONT_CPP})

# GPU backend (sokol_gfx, vendored in third_party/sokol): D3D11 on Windows,
# WebGL2 on the web, OpenGL ES 3 on Android, OpenGL ES 3 over EGL on Linux when the EGL/GLES
# development files are installed (headless: editor viewport and GPU
# screenshots). Without one the engine uses the software renderer only.
option(OE_GPU "Build the GPU renderer backend where available" ON)
set(OE_GPU_BACKEND "none")
if(OE_PLATFORM STREQUAL "win32")
  set(OE_PLATFORM_SOURCES engine/platform/win32/PlatformWin32.cpp engine/platform/win32/GpuD3D11.cpp)
  set(OE_GPU_BACKEND "d3d11")
elseif(OE_PLATFORM STREQUAL "web")
  set(OE_PLATFORM_SOURCES engine/platform/web/PlatformWeb.cpp engine/platform/web/GpuWebGL.cpp engine/platform/gl/SokolGL.cpp)
  set(OE_GPU_BACKEND "webgl2")
elseif(OE_PLATFORM STREQUAL "android")
  set(OE_PLATFORM_SOURCES engine/platform/android/PlatformAndroid.cpp engine/platform/android/GpuAndroid.cpp engine/platform/gl/SokolGL.cpp)
  set(OE_GPU_BACKEND "gles3-android")
  # NativeActivity glue from the NDK (android_main runs on its own thread).
  add_library(oe_android_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
  target_include_directories(oe_android_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
  target_compile_options(oe_android_glue PRIVATE -w)
else()
  set(OE_PLATFORM_SOURCES engine/platform/null/PlatformNull.cpp)
  if(OE_GPU)
    find_library(OE_EGL_LIBRARY EGL)
    find_library(OE_GLES_LIBRARY GLESv2)
    find_path(OE_EGL_INCLUDE EGL/egl.h)
    find_path(OE_GLES_INCLUDE GLES3/gl32.h)
  endif()
  if(OE_GPU AND OE_EGL_LIBRARY AND OE_GLES_LIBRARY AND OE_EGL_INCLUDE AND OE_GLES_INCLUDE)
    list(APPEND OE_PLATFORM_SOURCES engine/platform/null/GpuEgl.cpp engine/platform/gl/SokolGL.cpp)
    set(OE_GPU_BACKEND "gles3-egl")
  else()
    list(APPEND OE_PLATFORM_SOURCES engine/platform/null/GpuNone.cpp)
  endif()
endif()
message(STATUS "OwnEngine GPU backend: ${OE_GPU_BACKEND}")

list(APPEND OE_PLATFORM_SOURCES engine/platform/Network.cpp)
if(EMSCRIPTEN)
  list(APPEND OE_PLATFORM_SOURCES engine/platform/web/NetworkWeb.cpp)
else()
  list(APPEND OE_PLATFORM_SOURCES engine/platform/net/NativeSockets.cpp)
endif()

add_library(OwnEngine STATIC ${OE_CORE_SOURCES} ${OE_PLATFORM_SOURCES})
target_include_directories(OwnEngine PUBLIC engine third_party/sokol)
target_link_libraries(OwnEngine PUBLIC oe_lua Jolt oe_box2d)
# Single-header decoders (stb_image, cgltf); implementations in engine/assets/ThirdPartyImpl.cpp.
target_include_directories(OwnEngine PRIVATE third_party/stb third_party/cgltf)
target_compile_definitions(OwnEngine PUBLIC
  OE_VERSION="${PROJECT_VERSION}"
  OE_PLATFORM_NAME="${OE_PLATFORM}"
  OE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

if(MSVC)
  target_compile_options(OwnEngine PUBLIC /W4 /permissive- /utf-8 /wd4100 /wd4201)
  target_compile_definitions(OwnEngine PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(OwnEngine PUBLIC -Wall -Wextra -Wno-unused-parameter)
endif()

if(WIN32)
  target_link_libraries(OwnEngine PUBLIC ws2_32 bcrypt user32 gdi32 shell32 winmm d3d11 dxgi)
elseif(EMSCRIPTEN)
  # Single-threaded WebAssembly: no pthreads (works on any static host).
  # Bounded WebSocket JS callbacks copy at most 64 KiB through explicitly exported allocation functions.
  target_link_options(OwnEngine PUBLIC "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free']")
elseif(ANDROID)
  # Threads are part of bionic's libc.
  target_link_libraries(OwnEngine PUBLIC oe_android_glue android log EGL GLESv3 aaudio)
  target_compile_options(OwnEngine PUBLIC -ffp-contract=off)  # see oe_lua
else()
  find_package(Threads REQUIRED)
  target_link_libraries(OwnEngine PUBLIC Threads::Threads)
  if(OE_GPU_BACKEND STREQUAL "gles3-egl")
    target_link_libraries(OwnEngine PUBLIC ${OE_EGL_LIBRARY} ${OE_GLES_LIBRARY})
  endif()
endif()

if(EMSCRIPTEN)
  # ---------------------------------------------------------------------------
  # Web build (build_web.sh / build_web.bat): only the player runtime, as
  # oe_player.js + oe_player.wasm. `oe package --web` combines them with
  # tools/player/web/index.html and the game data (game.pak).
  # C++ exceptions (ApiError, Lua errors) use native WebAssembly exceptions.
  # ---------------------------------------------------------------------------
  foreach(t OwnEngine oe_lua Jolt)
    target_compile_options(${t} PUBLIC -fwasm-exceptions)
  endforeach()
  add_executable(oe_player tools/player/main.cpp)
  target_link_libraries(oe_player PRIVATE OwnEngine)
  target_link_options(oe_player PRIVATE
    -fwasm-exceptions
    -sENVIRONMENT=web
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sSTACK_SIZE=4MB
    -sFORCE_FILESYSTEM=1
    "-sEXPORTED_RUNTIME_METHODS=['FS','addRunDependency','removeRunDependency']"
    -sEXIT_RUNTIME=0)
  if(OE_BUILD_TESTS)
    # Self tests under Node.js with the real file system (proves the WASM
    # build simulates exactly like the native one): node build-web/bin/oe_tests.js
    add_executable(oe_tests tests/tests.cpp)
    target_link_libraries(oe_tests PRIVATE OwnEngine)
    target_link_options(oe_tests PRIVATE -fwasm-exceptions -sENVIRONMENT=node -sNODERAWFS=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=4MB -sEXIT_RUNTIME=1)
  endif()
  return()
endif()

if(ANDROID)
  # ---------------------------------------------------------------------------
  # Android build (build_android.sh / build_android.bat): only the player
  # runtime, as liboe_player.so for one ABI (NativeActivity loads it by the
  # name in AndroidManifest.xml). `oe package --android` puts it into an APK
  # with the game data. Static libc++ (ANDROID_STL=c++_static): one .so per ABI.
  # ---------------------------------------------------------------------------
  add_library(oe_player SHARED tools/player/main.cpp)
  target_link_libraries(oe_player PRIVATE OwnEngine)
  set_target_properties(oe_player PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  target_link_options(oe_player PRIVATE
    -u oe_ANativeActivity_onCreate  # entry point named in the manifest (only the system calls it)
    -Wl,-z,max-page-size=16384    # loads on devices with 16 KB pages (Android 15+)
    -Wl,--gc-sections)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_STRIP)
    add_custom_command(TARGET oe_player POST_BUILD COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:oe_player>)
  endif()
  if(OE_BUILD_TESTS)
    # Run through adb shell with source fixtures staged on the device.
    # GPU tests skip without a NativeActivity window; simulation tests still run.
    add_executable(oe_tests tests/tests.cpp)
    target_link_libraries(oe_tests PRIVATE OwnEngine)
  endif()
  return()
endif()

# ---------------------------------------------------------------------------
# Native editor (`oe editor`): Dear ImGui (docking) + ImGuizmo drawn with
# sokol_gfx next to the engine's GpuRenderer in one process. Needs a GPU
# backend; games (oe_player) never link it. engine/editor/ uses only the
# command API.
# ---------------------------------------------------------------------------
if(NOT OE_GPU_BACKEND STREQUAL "none")
  add_library(oe_imgui STATIC
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_stdlib.cpp third_party/imguizmo/ImGuizmo.cpp
    third_party/imguicolortextedit/TextEditor.cpp)
  # SYSTEM: no warnings from the vendored headers in engine code (MSVC /W4 included)
  target_include_directories(oe_imgui SYSTEM PUBLIC third_party/imgui third_party/imguizmo third_party/imguicolortextedit)
  target_compile_definitions(oe_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
  if(MSVC)
    target_compile_options(oe_imgui PRIVATE /W0 /utf-8)
  else()
    target_compile_options(oe_imgui PRIVATE -w)
  endif()
  if(WIN32)
    target_link_libraries(oe_imgui PUBLIC imm32)  # IME candidate window placement (Korean/CJK input)
  endif()

  file(GLOB OE_EDITOR_SOURCES CONFIGURE_DEPENDS engine/editor/*.cpp)
  add_library(oe_editor STATIC ${OE_EDITOR_SOURCES})
  target_link_libraries(oe_editor PUBLIC OwnEngine oe_imgui)
  target_compile_definitions(oe_editor PRIVATE SOKOL_IMGUI_NO_SOKOL_APP)  # input comes from engine/platform
  # sokol_imgui.h embeds one shader per graphics API: pick the engine's.
  if(OE_GPU_BACKEND STREQUAL "d3d11")
    target_compile_definitions(oe_editor PRIVATE SOKOL_D3D11)
  else()
    target_compile_definitions(oe_editor PRIVATE SOKOL_GLES3)
  endif()
  set(OE_NATIVE_EDITOR ON)
endif()

# Command line front-end: `oe` (run, render, exec, editor, mcp, ...)
add_executable(oe tools/oe/main.cpp)
target_link_libraries(oe PRIVATE OwnEngine)
if(OE_NATIVE_EDITOR)
  target_link_libraries(oe PRIVATE oe_editor)
  target_compile_definitions(oe PRIVATE OE_NATIVE_EDITOR=1)
endif()

# Game player runtime that `oe package` copies as <Game>.exe: a windowed app
# (no console) with a standard main().
add_executable(oe_player WIN32 tools/player/main.cpp)
target_link_libraries(oe_player PRIVATE OwnEngine)
if(MSVC)
  target_link_options(oe_player PRIVATE /ENTRY:mainCRTStartup)
endif()
add_dependencies(oe oe_player)

# Ship the project templates and the web player page next to the
# executable (runs every build so edits are picked up even when oe.exe does
# not relink). The web runtime itself (web/oe_player.js + .wasm) is added by
# build_web.sh / build_web.bat.
add_custom_target(oe_runtime_files ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/templates ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/templates
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/web
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/tools/player/web/index.html ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/web/index.html
  COMMENT "Copying project templates and the web player page")

if(OE_BUILD_TESTS)
  add_executable(oe_tests tests/tests.cpp)
  target_link_libraries(oe_tests PRIVATE OwnEngine)
  if(OE_NATIVE_EDITOR)
    target_link_libraries(oe_tests PRIVATE oe_editor)
    target_compile_definitions(oe_tests PRIVATE OE_NATIVE_EDITOR=1)
  endif()
  enable_testing()
  add_test(NAME oe_tests COMMAND oe_tests)
endif()
