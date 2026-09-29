cmake_minimum_required(VERSION 3.20)
project(OwnEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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

file(GLOB OE_CORE_SOURCES CONFIGURE_DEPENDS
  engine/core/*.cpp
  engine/scene/*.cpp
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
  target_link_libraries(OwnEngine PUBLIC ws2_32 user32 gdi32 shell32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(OwnEngine PUBLIC Threads::Threads)
endif()

# Command line front-end: `oe` (run, render, exec, editor, mcp, ...)
add_executable(oe tools/oe/main.cpp)
target_link_libraries(oe PRIVATE OwnEngine)

# Ship the web editor next to the executable.
add_custom_command(TARGET oe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/editor $<TARGET_FILE_DIR:oe>/editor)

if(OE_BUILD_TESTS)
  add_executable(oe_tests tests/tests.cpp)
  target_link_libraries(oe_tests PRIVATE OwnEngine)
  enable_testing()
  add_test(NAME oe_tests COMMAND oe_tests)
endif()
