cmake_minimum_required(VERSION 3.24)
project(zwift_vr LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ZwiftApp.exe is x64 - configure with -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL is loaded into Zwift, whose own directory is the DLL search path,
# so it must not depend on anything that isn't a system DLL.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# OpenXR loader, linked statically for the same reason.
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_STATIC_CRT ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
  GIT_TAG release-1.1.63
  GIT_SHALLOW TRUE)

FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG v1.3.4
  GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(openxr minhook)
# The SDK forces /MD on a *static* loader (src/loader/CMakeLists.txt); we need /MT throughout.
set_target_properties(openxr_loader minhook PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- the injected DLL ----
add_library(zwift_vr SHARED
  src/dll/dllmain.cpp
  src/dll/camera.cpp
  src/dll/profiler.cpp
  src/dll/log.cpp
  src/dll/config.cpp
  src/dll/gl_capture.cpp
  src/dll/gl_trace.cpp
  src/dll/vr_window.cpp
  src/dll/xr_thread.cpp
  src/dll/zwift_state.cpp)
target_compile_definitions(zwift_vr PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
  XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11)
target_link_libraries(zwift_vr PRIVATE
  openxr_loader minhook opengl32 d3d11 dxgi d3dcompiler winmm)
target_compile_options(zwift_vr PRIVATE /W4 /permissive-)
# Linker map so profiler RVAs inside zwift_vr.dll can be named (build/zwift_vr.map).
target_link_options(zwift_vr PRIVATE "/MAP:${CMAKE_BINARY_DIR}/zwift_vr.map")

# ---- the injector (needs admin: ZwiftApp.exe runs elevated) ----
add_executable(zvr_loader src/loader/loader.cpp)
target_compile_definitions(zvr_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_options(zvr_loader PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
target_compile_options(zvr_loader PRIVATE /W4 /permissive-)

# Keep everything the user runs in one folder: build/<Config>/
set_target_properties(zwift_vr zvr_loader PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>)
add_custom_command(TARGET zwift_vr POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/config/zwift_vr.ini $<TARGET_FILE_DIR:zwift_vr>/zwift_vr.ini)
