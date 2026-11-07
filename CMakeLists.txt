cmake_minimum_required(VERSION 3.24)
project(GunSmokeCoopPatcher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# One self-contained exe: the C runtime and SDL linked statically.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT WIN32)
  message(FATAL_ERROR "The patcher's window uses Windows GDI; build it on Windows (MSVC).")
endif()

# No build machine paths in the exe: __FILE__ (SDL's asserts) relative to the source and build folders.
if(MSVC)
  file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" GS_BIN_NATIVE)
  file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" GS_SRC_NATIVE)
  add_compile_options("/d1trimfile:${GS_BIN_NATIVE}" "/d1trimfile:${GS_SRC_NATIVE}")
endif()

# SDL 3: an installed copy (SDL3_DIR / CMAKE_PREFIX_PATH), else downloaded and linked statically.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz)
  FetchContent_MakeAvailable(SDL3)
endif()
if(TARGET SDL3::SDL3)
  set(GS_SDL SDL3::SDL3)
else()
  set(GS_SDL SDL3::SDL3-static)
endif()

if(MSVC)
  set(GS_WARNINGS /W4 /utf-8)
else()
  set(GS_WARNINGS -Wall -Wextra)
endif()

# The NES picture unit and the game's engine (draws the window's stage 1 picture from the user's ROM).
add_library(gs_nes STATIC src/compat/nes/ppu.cpp src/compat/nes/machine.cpp src/compat/nes/apu.cpp)
target_include_directories(gs_nes PUBLIC src)
target_compile_options(gs_nes PRIVATE ${GS_WARNINGS})

add_library(gs_engine STATIC games/gunsmoke/engine.cpp games/gunsmoke/sound.cpp)
target_include_directories(gs_engine PUBLIC games/gunsmoke)
target_link_libraries(gs_engine PUBLIC gs_nes)
target_compile_options(gs_engine PRIVATE ${GS_WARNINGS})

add_executable(gunsmoke_coop_patcher WIN32 tools/gunsmoke-coop/patcher.cpp)
target_link_libraries(gunsmoke_coop_patcher PRIVATE gs_engine ${GS_SDL} bcrypt shell32 ole32 gdi32 user32)
target_compile_options(gunsmoke_coop_patcher PRIVATE ${GS_WARNINGS})
set_target_properties(gunsmoke_coop_patcher PROPERTIES OUTPUT_NAME "GunSmoke-Coop-Patcher")
if(TARGET SDL3::SDL3)
  add_custom_command(TARGET gunsmoke_coop_patcher POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:gunsmoke_coop_patcher>)
endif()
