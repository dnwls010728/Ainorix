cmake_minimum_required(VERSION 3.20)
project(OwnEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Static C/C++ runtime: packaged games run on PCs without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(OE_BUILD_TESTS "Build the engine test executable" ON)

# ---------------------------------------------------------------------------
# Platform selection. Every platform implements engine/platform/Platform.h.
# Only Win32 and Null (headless) exist today; the others are planned and
# listed in docs/PLATFORMS.md.
# ---------------------------------------------------------------------------
if(WIN32)
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

file(GLOB OE_CORE_SOURCES CONFIGURE_DEPENDS
  engine/core/*.cpp
  engine/scene/*.cpp
  engine/script/*.cpp
  engine/physics/*.cpp
  engine/audio/*.cpp
  engine/assets/*.cpp
  engine/render/*.cpp
  engine/api/*.cpp
  engine/app/*.cpp)

if(OE_PLATFORM STREQUAL "win32")
  set(OE_PLATFORM_SOURCES engine/platform/win32/PlatformWin32.cpp)
else()
  set(OE_PLATFORM_SOURCES engine/platform/null/PlatformNull.cpp)
endif()

add_library(OwnEngine STATIC ${OE_CORE_SOURCES} ${OE_PLATFORM_SOURCES})
target_include_directories(OwnEngine PUBLIC engine)
target_link_libraries(OwnEngine PUBLIC oe_lua Jolt)
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
  target_link_libraries(OwnEngine PUBLIC ws2_32 user32 gdi32 shell32 winmm)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(OwnEngine PUBLIC Threads::Threads)
endif()

# Command line front-end: `oe` (run, render, exec, editor, mcp, ...)
add_executable(oe tools/oe/main.cpp)
target_link_libraries(oe PRIVATE OwnEngine)

# Game player runtime that `oe package` copies as <Game>.exe: a windowed app
# (no console) with a standard main().
add_executable(oe_player WIN32 tools/player/main.cpp)
target_link_libraries(oe_player PRIVATE OwnEngine)
if(MSVC)
  target_link_options(oe_player PRIVATE /ENTRY:mainCRTStartup)
endif()
add_dependencies(oe oe_player)

# Ship the web editor next to the executable (runs every build so edits to
# editor/ are picked up even when oe.exe does not relink).
add_custom_target(oe_editor_files ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/editor ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/editor
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/templates ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/templates
  COMMENT "Copying web editor and project templates")

if(OE_BUILD_TESTS)
  add_executable(oe_tests tests/tests.cpp)
  target_link_libraries(oe_tests PRIVATE OwnEngine)
  enable_testing()
  add_test(NAME oe_tests COMMAND oe_tests)
endif()
