cmake_minimum_required(VERSION 3.20)
project(OnlineRPGServers LANGUAGES C CXX)

# The back end of games/OnlineRPG: programs that are not the engine and not game data
# (docs/ONLINERPG_SERVERS.md). Built on their own: `build.bat` here, or
#   cmake -S games/OnlineRPG/server -B build/server -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/server

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

find_package(Threads REQUIRED)

# SQLite amalgamation, unmodified (vendor/sqlite/README.md). One connection per database, used by
# one thread at a time: the multi-thread mode without per-connection mutexes is enough.
add_library(rpg_sqlite STATIC vendor/sqlite/sqlite3.c)
target_include_directories(rpg_sqlite PUBLIC vendor/sqlite)
target_compile_definitions(rpg_sqlite PRIVATE
  SQLITE_THREADSAFE=2 SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_DEPRECATED SQLITE_OMIT_SHARED_CACHE SQLITE_LIKE_DOESNT_MATCH_BLOBS SQLITE_USE_ALLOCA)
if(MSVC)
  target_compile_options(rpg_sqlite PRIVATE /w)
else()
  target_compile_options(rpg_sqlite PRIVATE -w)
  target_link_libraries(rpg_sqlite PUBLIC ${CMAKE_DL_LIBS})
endif()

add_library(rpg_common STATIC
  common/Crypto.cpp common/Net.cpp common/Store.cpp common/Ticket.cpp common/Wire.cpp
  login/LoginServer.cpp channel/ChannelServer.cpp)
target_include_directories(rpg_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rpg_common PUBLIC rpg_sqlite Threads::Threads)
if(MSVC)
  target_compile_options(rpg_common PUBLIC /W4 /permissive- /utf-8)
  target_compile_definitions(rpg_common PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(rpg_common PUBLIC -Wall -Wextra)
endif()
if(WIN32)
  target_link_libraries(rpg_common PUBLIC ws2_32 bcrypt)
endif()

add_executable(oe_login login/main.cpp)
target_link_libraries(oe_login PRIVATE rpg_common)

add_executable(oe_channel channel/main.cpp)
target_link_libraries(oe_channel PRIVATE rpg_common)

add_executable(oe_server_tests test/main.cpp)
target_link_libraries(oe_server_tests PRIVATE rpg_common)
