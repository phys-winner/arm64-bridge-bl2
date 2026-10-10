cmake_minimum_required(VERSION 3.22)
project(arm32_bridge LANGUAGES C CXX)

# Rename the target to the .so name the game's Java side loads with
# System.loadLibrary(). Nothing else in the build depends on the name.
set(BRIDGE_TARGET UnrealEngine3)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(HORN_ENABLE_PERF_PROBE "Compile on-device performance diagnostics" OFF)
option(HORN_ENABLE_LOGGING "Compile bridge and forwarded guest logcat output" OFF)
option(HORN_ENABLE_PHYSX_COOKIE_WATCH "Compile optional default-off BL2 PhysX first-write diagnostic" OFF)
option(HORN_GL_SELF_CHECK "Compile bounded real GLES guest-thunk regression" OFF)
option(BL2_ENABLE_DEBUG_MENU_DEFAULT "Show Pause Debug by default in development builds" OFF)

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

# FETCHCONTENT_SOURCE_DIR skips PATCH_COMMAND for the pinned offline checkout.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${dynarmic_SOURCE_DIR}"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-arm64-fast-dispatch.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
    RESULT_VARIABLE dispatch_patch_result)
if(NOT dispatch_patch_result EQUAL 0)
    message(FATAL_ERROR "Fast dispatch patch is incompatible with the selected Dynarmic")
endif()

# Apply the measured memory fix there too; a different tree must match its patch.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${dynarmic_SOURCE_DIR}"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-arm64-fastmem-fpsr.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
    RESULT_VARIABLE fpsr_patch_result)
if(NOT fpsr_patch_result EQUAL 0)
    message(FATAL_ERROR "Fastmem FPSR patch is incompatible with the selected Dynarmic")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${dynarmic_SOURCE_DIR}"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-arm64-vmov-select.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
    RESULT_VARIABLE vmov_patch_result)
if(NOT vmov_patch_result EQUAL 0)
    message(FATAL_ERROR "Conditional VMOV patch is incompatible with the selected Dynarmic")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${dynarmic_SOURCE_DIR}"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-arm64-fpcr-cluster.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
    RESULT_VARIABLE fpcr_patch_result)
if(NOT fpcr_patch_result EQUAL 0)
    message(FATAL_ERROR "Vector FPCR patch is incompatible with the selected Dynarmic")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${dynarmic_SOURCE_DIR}"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-arm64-vldr-literal.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
    RESULT_VARIABLE literal_patch_result)
if(NOT literal_patch_result EQUAL 0)
    message(FATAL_ERROR "VLDR literal patch is incompatible with the selected Dynarmic")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${dynarmic_SOURCE_DIR}"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-arm64-jit-compile-probe.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
    RESULT_VARIABLE compile_probe_patch_result)
if(NOT compile_probe_patch_result EQUAL 0)
    message(FATAL_ERROR "JIT compile probe patch is incompatible with the selected Dynarmic")
endif()
if(HORN_ENABLE_PERF_PROBE)
    target_compile_definitions(dynarmic PRIVATE HORN_ENABLE_JIT_COMPILE_PROBE=1)
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${dynarmic_SOURCE_DIR}"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-arm64-precompile-api.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
    RESULT_VARIABLE precompile_patch_result)
if(NOT precompile_patch_result EQUAL 0)
    message(FATAL_ERROR "Precompile API patch is incompatible with the selected Dynarmic")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${dynarmic_SOURCE_DIR}"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-arm64-a32-write-watch.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
    RESULT_VARIABLE write_watch_patch_result)
if(NOT write_watch_patch_result EQUAL 0)
    message(FATAL_ERROR "A32 write-watch patch is incompatible with the selected Dynarmic")
endif()

# Emitted-block observer and callback-time precompile for the learned warmup.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${dynarmic_SOURCE_DIR}"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-arm64-learned-warmup.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
    RESULT_VARIABLE learned_warmup_patch_result)
if(NOT learned_warmup_patch_result EQUAL 0)
    message(FATAL_ERROR "Learned warmup patch is incompatible with the selected Dynarmic")
endif()

# An unattached coprocessor (a guest jump into data decodes LDC/STC) raises the
# guest undefined-instruction exception instead of aborting while compiling.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${dynarmic_SOURCE_DIR}"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-arm64-coprocessor-undefined.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
    RESULT_VARIABLE coprocessor_patch_result)
if(NOT coprocessor_patch_result EQUAL 0)
    message(FATAL_ERROR "Coprocessor undefined patch is incompatible with the selected Dynarmic")
endif()

# Reuse the Warband fastmem/FPSCR regression against this project's Dynarmic.
add_executable(bl2_fastmem_fpsr_test EXCLUDE_FROM_ALL tests/fastmem_fpsr_test.cpp)
target_link_libraries(bl2_fastmem_fpsr_test PRIVATE dynarmic)
target_link_options(bl2_fastmem_fpsr_test PRIVATE "-Wl,-z,max-page-size=16384")

add_executable(bl2_shader_warmup_test EXCLUDE_FROM_ALL tests/bl2_shader_warmup_test.cpp)
target_include_directories(bl2_shader_warmup_test PRIVATE src)
target_link_options(bl2_shader_warmup_test PRIVATE "-Wl,-z,max-page-size=16384")

add_executable(bl2_conditional_vmov_test EXCLUDE_FROM_ALL tests/conditional_vmov_selfcheck.cpp)
target_link_libraries(bl2_conditional_vmov_test PRIVATE dynarmic)
target_link_options(bl2_conditional_vmov_test PRIVATE "-Wl,-z,max-page-size=16384")

add_executable(bl2_precompile_api_test EXCLUDE_FROM_ALL tests/precompile_api_selfcheck.cpp)
target_link_libraries(bl2_precompile_api_test PRIVATE dynarmic fmt::fmt mcl)
target_link_options(bl2_precompile_api_test PRIVATE "-Wl,-z,max-page-size=16384"
                    "-Wl,--wrap=__system_property_get")

add_executable(bl2_jit_warmup_test EXCLUDE_FROM_ALL tests/bl2_jit_warmup_test.cpp)
target_include_directories(bl2_jit_warmup_test PRIVATE src)
target_link_options(bl2_jit_warmup_test PRIVATE "-Wl,-z,max-page-size=16384")

add_executable(bl2_fpcr_literal_test EXCLUDE_FROM_ALL tests/fpcr_literal_selfcheck.cpp)
target_link_libraries(bl2_fpcr_literal_test PRIVATE dynarmic fmt::fmt mcl)
target_link_options(bl2_fpcr_literal_test PRIVATE "-Wl,-z,max-page-size=16384"
                    "-Wl,--wrap=__system_property_get")

add_executable(bl2_software_write_watch_test EXCLUDE_FROM_ALL tests/software_write_watch_selfcheck.cpp)
target_link_libraries(bl2_software_write_watch_test PRIVATE dynarmic fmt::fmt mcl)
target_link_options(bl2_software_write_watch_test PRIVATE "-Wl,-z,max-page-size=16384"
                    "-Wl,--wrap=__system_property_get")

add_executable(bl2_gl_storage_control EXCLUDE_FROM_ALL tests/gl_storage_control.cpp)
target_link_libraries(bl2_gl_storage_control PRIVATE EGL GLESv3)
target_link_options(bl2_gl_storage_control PRIVATE "-Wl,-z,max-page-size=16384")

set(BRIDGE_SOURCES
    src/elf_loader.cpp
    src/dynarmic_host.cpp
    src/engine_hooks.cpp
    src/guest_mapped_dirty_tracker.cpp
    src/guest_software_write_watch.cpp
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
    src/bl2_touch_inventory_gesture.cpp
    src/bl2_touch_text_chat.cpp
    src/bl2_touch_ui_model.cpp
    src/bl2_touch_ui_model_probe.cpp
    src/bl2_touch_gfx_values.cpp
    src/bl2_touch_gfx_transport.cpp
    src/bl2_touch_widget_registry.cpp
    src/bl2_touch_widget_gestures.cpp
    src/bl2_touch_widget_commands.cpp
    src/bl2_touch_widget_router.cpp
    src/bl2_touch_widget_sink.cpp
    src/thunks_gl.cpp
    src/thunks_gl_gles2_core.cpp
    src/thunks_android.cpp
    src/thunks_libc.cpp
    src/thunks_tegra.cpp
    src/thunks_opensles.cpp   # Horn resolves libOpenSLES.so dynamically; keep this bridge
    src/jni_fake_env.cpp
    src/jni_bridge.cpp
    src/bl2_native_resolution.cpp
    src/bl2_graphics_settings.cpp
    src/bl2_fresh_buffer_names.cpp
    src/bl2_pause_debug_menu.cpp
    src/bl2_vertex_element_fastpath.cpp
    src/bl2_convex_fastpath.cpp
    src/bl2_limiter_peak_fastpath.cpp
    src/bl2_program_locals_fastpath.cpp
    src/bl2_performance_settings.cpp
    src/bl2_audio_decode_fastpath.cpp
    src/bl2_perf_hint.cpp
    src/bl2_program_upgrade.cpp
    src/bl2_jit_learning.cpp
    src/bl2_hitch_report.cpp
    src/bl2_guest_integrity.cpp
)

if(HORN_ENABLE_PERF_PROBE)
    list(APPEND BRIDGE_SOURCES src/perf_probe.cpp)
endif()

add_library(${BRIDGE_TARGET} SHARED ${BRIDGE_SOURCES})
if(HORN_GL_SELF_CHECK)
    target_compile_definitions(${BRIDGE_TARGET} PRIVATE HORN_GL_SELF_CHECK=1)
    add_executable(bl2_gl_parity_selfcheck EXCLUDE_FROM_ALL tests/gl_parity_selfcheck.cpp)
    target_link_libraries(bl2_gl_parity_selfcheck PRIVATE ${BRIDGE_TARGET})
    target_link_options(bl2_gl_parity_selfcheck PRIVATE "-Wl,-z,max-page-size=16384")
    add_executable(bl2_fresh_buffer_names_selfcheck EXCLUDE_FROM_ALL tests/bl2_fresh_buffer_names_selfcheck.cpp)
    target_link_libraries(bl2_fresh_buffer_names_selfcheck PRIVATE ${BRIDGE_TARGET} EGL)
    target_link_options(bl2_fresh_buffer_names_selfcheck PRIVATE "-Wl,-z,max-page-size=16384")
endif()
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
target_compile_definitions(${BRIDGE_TARGET} PRIVATE
    BL2_ENABLE_DEBUG_MENU_DEFAULT=$<BOOL:${BL2_ENABLE_DEBUG_MENU_DEFAULT}>)
target_compile_definitions(bl2_touch_sources PRIVATE
    BL2_ENABLE_DEBUG_MENU_DEFAULT=$<BOOL:${BL2_ENABLE_DEBUG_MENU_DEFAULT}>)

if(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    target_compile_definitions(${BRIDGE_TARGET} PRIVATE HORN_ENABLE_PHYSX_COOKIE_WATCH=1)
endif()

if(HORN_ENABLE_PERF_PROBE)
    target_compile_definitions(${BRIDGE_TARGET} PRIVATE HORN_ENABLE_PERF_PROBE=1)
endif()

if(HORN_ENABLE_LOGGING OR HORN_ENABLE_PHYSX_COOKIE_WATCH)
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
    log  # Bounded backend/cache/display witnesses also survive quiet builds.
    m
    z
)

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
