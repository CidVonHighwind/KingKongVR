cmake_minimum_required(VERSION 3.20)
project(kingkong_vr VERSION 0.1.0 LANGUAGES CXX)

# The game is a 32-bit process, so the proxy must be too. Configure with:
#   cmake -S . -B build -A Win32
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "This must be built as 32-bit to match the game. Configure with -A Win32.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT everywhere (including the OpenXR loader) so the DLL drops next to
# a 2005 game with no redist needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# OpenXR loader, built from source as a static library. Khronos only ships a
# prebuilt loader as a NuGet package.
include(FetchContent)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
  GIT_TAG release-1.1.63
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(openxr)
# The loader forces the DLL CRT for static builds; override to match ours.
set_target_properties(openxr_loader PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Regenerate the COM wrapper method lists from the Windows SDK header.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GENERATED
  ${CMAKE_CURRENT_SOURCE_DIR}/src/device/generated/idirect3d9_methods.inl
  ${CMAKE_CURRENT_SOURCE_DIR}/src/device/generated/idirect3ddevice9_methods.inl)
add_custom_command(
  OUTPUT ${GENERATED}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_wrapper.py
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_wrapper.py
  COMMENT "Generating COM wrapper method lists from the Windows SDK"
  VERBATIM)

add_library(d3d9 SHARED
  # device: the d3d9.dll export and the wrapped Direct3D 9 device
  src/device/proxy.cpp
  src/device/wrapper.cpp
  src/device/lock_hooks.cpp
  src/device/d3d9.def
  # record: the game's frame as a recording and a model of its device state
  src/record/recording.cpp
  src/record/game_state.cpp
  src/record/vertex_arena.cpp
  # render: each eye rendered from the recording, output to the headset
  src/render/frame_render.cpp
  src/render/eye_renderer.cpp
  src/render/shared_output.cpp
  src/render/preview.cpp
  # xr: the OpenXR thread
  src/xr/xr.cpp
  # game: patches of the game executable
  src/game/aspect.cpp
  src/game/timing.cpp
  src/game/joystick.cpp
  # common: settings, log, crash reports
  src/common/config.cpp
  src/common/log.cpp
  src/common/crash.cpp
  # debug: test automation and diagnostics
  src/debug/autotest.cpp
  src/debug/glitch_catcher.cpp
  src/debug/frame_times.cpp
  ${GENERATED})

target_include_directories(d3d9 PRIVATE src)
target_link_libraries(d3d9 PRIVATE d3d9.lib d3d11.lib dxgi.lib dxguid.lib
  dbghelp.lib winmm.lib xinput.lib
  openxr_loader)
set_target_properties(d3d9 PROPERTIES OUTPUT_NAME "d3d9" PREFIX "")

if(MSVC)
  target_compile_options(d3d9 PRIVATE /W4 /permissive- /MP)
  # A map file resolves crash offsets logged by src/common/crash.cpp.
  target_link_options(d3d9 PRIVATE /MAP)
  target_compile_definitions(d3d9 PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
# The mod's version, in the log and the release package name
# (tools/package.ps1 reads it from here).
target_compile_definitions(d3d9 PRIVATE KKVR_VERSION="${PROJECT_VERSION}")
