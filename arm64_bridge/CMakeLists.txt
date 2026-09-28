cmake_minimum_required(VERSION 3.22)
project(arm32_bridge LANGUAGES C CXX)

# Rename the target to the .so name the game's Java side loads with
# System.loadLibrary(). Nothing else in the build depends on the name.
set(BRIDGE_TARGET UnrealEngine3)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(HORN_ENABLE_PERF_PROBE "Compile on-device performance diagnostics" OFF)
option(HORN_ENABLE_LOGGING "Compile bridge and forwarded guest logcat output" OFF)

include(FetchContent)

# Size: applied before FetchContent so dynarmic/fmt/mcl inherit them. Only
# JNI_OnLoad has to be exported (everything else is registered at runtime), so
# hiding the rest drops the dynsym table and most of .rela.
add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden
                    -ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections -Wl,--icf=all -Wl,--exclude-libs,ALL)

# Boost Headers
set(BOOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost")
set(BOOST_INCLUDEDIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost")
set(Boost_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost")
set(Boost_NO_SYSTEM_PATHS ON)
set(Boost_FOUND TRUE)
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost")

# Configure Dynarmic.
# NOTE: the real option is DYNARMIC_FRONTENDS (a list), not DYNARMIC_ENABLE_A32/A64 --
# the old names were silently ignored, so A64 was being built too.
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)  # cross-compiling: never pick up host libs

# Keep the production build pinned and offline. Experiments may select an
# explicit, separately pinned source tree without mutating the production one.
set(HORN_DYNARMIC_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/_deps/dynarmic-src"
    CACHE PATH "Explicit Dynarmic source tree")
set(FETCHCONTENT_SOURCE_DIR_DYNARMIC
    "${HORN_DYNARMIC_SOURCE}"
    CACHE PATH "Local pinned Dynarmic source" FORCE)
message(STATUS "Using explicit Dynarmic source: ${HORN_DYNARMIC_SOURCE}")
if(HORN_DYNARMIC_SOURCE STREQUAL
   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/_deps/dynarmic-src")
    FetchContent_Declare(
        dynarmic
        GIT_REPOSITORY https://github.com/azahar-emu/dynarmic.git
        GIT_TAG        e77b1ba0b7da7cbe93021b01a663acfe7c4dd516
        PATCH_COMMAND
            "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=<SOURCE_DIR>"
            "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-arm64-fast-dispatch.patch"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
    )
else()
    FetchContent_Declare(dynarmic SOURCE_DIR "${HORN_DYNARMIC_SOURCE}")
endif()
FetchContent_MakeAvailable(dynarmic)

set(BRIDGE_SOURCES
    src/elf_loader.cpp
    src/dynarmic_host.cpp
    src/guest_mapped_dirty_tracker.cpp
    src/obb_split_inventory.cpp
    src/horn_fastpaths.cpp
    src/thunk_manager.cpp
    src/bl2_startup_profile.cpp
    src/bl2_directory_cache.cpp
    src/bl2_io_failure_probe.cpp
    src/bl2_fog_audit.cpp
    src/bl2_locale.cpp
    src/bl2_touch_runtime.cpp
    src/bl2_touch_gameplay_sink.cpp
    src/bl2_touch_gameplay_hooks.cpp
    src/bl2_touch_physical.cpp
    src/bl2_touch_ui_sink.cpp
    src/bl2_touch_ui_transactions.cpp
    src/bl2_touch_ui_model.cpp
    src/bl2_touch_ui_model_probe.cpp
    src/thunks_gl.cpp
    src/thunks_gl_gles2_core.cpp
    src/thunks_android.cpp
    src/thunks_libc.cpp
    src/thunks_tegra.cpp
    src/thunks_opensles.cpp   # Horn resolves libOpenSLES.so dynamically; keep this bridge
    src/jni_fake_env.cpp
    src/jni_bridge.cpp
    src/bl2_native_resolution.cpp
    src/bl2_pause_debug_menu.cpp
    src/bl2_vertex_element_fastpath.cpp
    src/bl2_convex_fastpath.cpp
)

if(HORN_ENABLE_PERF_PROBE)
    list(APPEND BRIDGE_SOURCES src/perf_probe.cpp)
endif()

add_library(${BRIDGE_TARGET} SHARED ${BRIDGE_SOURCES})
file(STRINGS "${dynarmic_SOURCE_DIR}/src/dynarmic/interface/A32/config.h"
     HORN_DYNARMIC_INTERPRETER_FALLBACK_DECL
     REGEX "virtual void InterpreterFallback")
if(HORN_DYNARMIC_INTERPRETER_FALLBACK_DECL)
    target_compile_definitions(${BRIDGE_TARGET} PRIVATE
        HORN_DYNARMIC_HAS_INTERPRETER_FALLBACK=1)
endif()
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../touch_native_design/arm64_bridge"
    "${CMAKE_CURRENT_BINARY_DIR}/bl2_touch")
target_link_libraries(${BRIDGE_TARGET} PRIVATE bl2_touch_sources)

if(HORN_ENABLE_PERF_PROBE)
    target_compile_definitions(${BRIDGE_TARGET} PRIVATE HORN_ENABLE_PERF_PROBE=1)
endif()

if(HORN_ENABLE_LOGGING)
    target_compile_definitions(${BRIDGE_TARGET} PRIVATE HORN_ENABLE_LOGGING=1)
endif()

target_include_directories(${BRIDGE_TARGET} PRIVATE
    src
    ${dynarmic_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost
)

target_link_libraries(${BRIDGE_TARGET} PRIVATE
    dynarmic
    GLESv3
    EGL
    OpenSLES
    android
    m
    z
)

if(HORN_ENABLE_LOGGING OR HORN_ENABLE_PERF_PROBE)
    target_link_libraries(${BRIDGE_TARGET} PRIVATE log)
endif()

target_compile_definitions(${BRIDGE_TARGET} PRIVATE
    ANDROID
    __ANDROID__
)

# Android 15+ devices boot with 16 KB pages; a 4 KB-aligned LOAD segment trips
# the "app isn't 16 KB compatible" loader warning.
target_link_options(${BRIDGE_TARGET} PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384"
)
