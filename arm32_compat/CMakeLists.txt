cmake_minimum_required(VERSION 3.22)
project(bl2_native_arm32 LANGUAGES C CXX ASM)
if(NOT ANDROID_ABI STREQUAL "armeabi-v7a")
    message(FATAL_ERROR "This compatibility library targets native armeabi-v7a")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(HORN_ENABLE_LOGGING "Compile bounded native compatibility diagnostics" OFF)
option(BL2_ENABLE_QA_CONSOLE "Compile the opt-in QA console without other diagnostics" OFF)
option(BL2_ENABLE_DLC_CONTENT "Load optional external DLC OBB content" ON)
option(BL2_ENABLE_DEBUG_MENU_DEFAULT "Show Pause Debug by default in development builds" OFF)
option(BL2_NATIVE_PERF_PROBE "Compile opt-in native NVSI/frame diagnostics" OFF)
set(COMMON "${CMAKE_CURRENT_SOURCE_DIR}/../arm64_bridge/src")
set(TOUCH "${CMAKE_CURRENT_SOURCE_DIR}/../touch_native_design/arm64_bridge")
file(GLOB TOUCH_RUNTIME "${COMMON}/bl2_touch_*.cpp")
list(REMOVE_ITEM TOUCH_RUNTIME "${COMMON}/bl2_touch_widget_probe.cpp")
add_library(UnrealEngine3 SHARED
    src/native_calls.S src/native_host.cpp src/native_elf_loader.cpp
    src/native_platform.cpp src/native_vfs.cpp src/native_jni.cpp src/native_bootstrap.cpp src/native_sha256.cpp
    src/native_vulkan_renderer.cpp src/native_vulkan_thunks.cpp src/native_vulkan_shader_pack.cpp src/native_vulkan_seed.cpp
    "${COMMON}/thunk_manager.cpp" "${COMMON}/thunks_gl.cpp" "${COMMON}/thunks_gl_gles2_core.cpp"
    "${COMMON}/thunks_tegra.cpp" "${COMMON}/obb_split_inventory.cpp"
    "${COMMON}/guest_mapped_dirty_tracker.cpp" "${COMMON}/guest_software_write_watch.cpp"
    "${COMMON}/bl2_native_resolution.cpp" "${COMMON}/bl2_fresh_buffer_names.cpp" "${COMMON}/bl2_graphics_settings.cpp"
    "${COMMON}/bl2_pause_debug_menu.cpp" "${COMMON}/bl2_locale.cpp" "${COMMON}/bl2_bink_audio.cpp"
    "${COMMON}/bl2_movie_skip.cpp" "${COMMON}/bl2_startup_screens.cpp"
    "${COMMON}/bl2_shader_former.cpp" "${COMMON}/bl2_program_upgrade.cpp" "${COMMON}/bl2_obb_package_watch.cpp"
    "${COMMON}/bl2_perf_hint.cpp" "${COMMON}/bl2_directory_cache.cpp"
    "${COMMON}/bl2_gc_defer.cpp" "${COMMON}/bl2_auto_flush.cpp"
    ${TOUCH_RUNTIME} "${TOUCH}/bl2_touch_input.cpp" "${TOUCH}/bl2_touch_engine_adapter.cpp" "${TOUCH}/bl2_touch_jni.cpp")
target_include_directories(UnrealEngine3 PUBLIC include src "${COMMON}" "${TOUCH}")
target_compile_definitions(UnrealEngine3 PUBLIC BL2_NATIVE_ARM32=1 _FILE_OFFSET_BITS=64
    BL2_NATIVE_PERF_PROBE=$<BOOL:${BL2_NATIVE_PERF_PROBE}>
    HORN_ENABLE_PERF_PROBE=$<BOOL:${BL2_NATIVE_PERF_PROBE}>
    HORN_ENABLE_LOGGING=$<BOOL:${HORN_ENABLE_LOGGING}>
    BL2_ENABLE_QA_CONSOLE=$<BOOL:${BL2_ENABLE_QA_CONSOLE}>
    BL2_ENABLE_DLC_CONTENT=$<BOOL:${BL2_ENABLE_DLC_CONTENT}>
    BL2_ENABLE_DEBUG_MENU_DEFAULT=$<BOOL:${BL2_ENABLE_DEBUG_MENU_DEFAULT}>)
if(BL2_NATIVE_PERF_PROBE)
    target_sources(UnrealEngine3 PRIVATE src/native_perf.cpp "${COMMON}/perf_probe.cpp")
    add_executable(native_perf_check EXCLUDE_FROM_ALL tests/native_perf_check.cpp)
    target_link_libraries(native_perf_check PRIVATE UnrealEngine3)
    target_link_options(native_perf_check PRIVATE -Wl,-z,max-page-size=16384)
endif()
# Shared handlers use a zero origin for real ARM32 addresses. Preserve that
# compiler-supported identity mapping; byte spans still validate their bounds.
target_compile_options(UnrealEngine3 PRIVATE -fno-delete-null-pointer-checks -ffunction-sections -fdata-sections -mfpu=neon)
target_link_options(UnrealEngine3 PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
target_link_libraries(UnrealEngine3 PRIVATE GLESv3 EGL vulkan android log m z dl)
set(vulkan_backend_inputs
    src/native_vulkan_renderer.cpp src/native_vulkan_renderer.h
    src/native_vulkan_thunks.cpp src/native_vulkan_thunks.h
    src/native_vulkan_shader_pack.cpp src/native_vulkan_shader_pack.h
    include/bl2_buffer_pool_core.h
    src/native_vulkan_seed.cpp src/native_vulkan_seed.h src/native_vfs.cpp src/native_vfs.h
    ../arm64_bridge/src/thunk_manager.cpp CMakeLists.txt
    src/native_jni.cpp src/native_jni.h
    ../arm64_bridge/src/bl2_graphics_settings.cpp ../arm64_bridge/src/bl2_graphics_settings.h)
set(vulkan_backend_hashes "")
foreach(input IN LISTS vulkan_backend_inputs)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${input}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    file(SHA256 "${source}" digest)
    string(APPEND vulkan_backend_hashes "${digest}")
endforeach()
string(SHA256 vulkan_backend_identity "${vulkan_backend_hashes}")
target_compile_definitions(UnrealEngine3 PRIVATE BL2_VULKAN_BACKEND_ID="${vulkan_backend_identity}")
# Disabled until a genuine successful fire/hit scope is approved.
option(BL2_VK_REQUIRE_SEED "Require the approved portable PSO seed before Vulkan dispatch" OFF)
foreach(identity CONTENT SCOPE RECEIPT)
    set(BL2_VK_SEED_${identity}_ID "" CACHE STRING "Approved seed ${identity} SHA256")
    if(BL2_VK_REQUIRE_SEED)
        string(LENGTH "${BL2_VK_SEED_${identity}_ID}" identity_length)
        if(NOT identity_length EQUAL 64 OR NOT BL2_VK_SEED_${identity}_ID MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR "Seed ${identity} identity must be exactly 64 lowercase hex digits")
        endif()
        target_compile_definitions(UnrealEngine3 PRIVATE BL2_VK_SEED_${identity}_ID="${BL2_VK_SEED_${identity}_ID}")
    endif()
endforeach()
target_compile_definitions(UnrealEngine3 PRIVATE BL2_VK_REQUIRE_SEED=$<BOOL:${BL2_VK_REQUIRE_SEED}>)
add_library(tsec_wrapper SHARED src/tsec_unavailable.c)
target_link_options(tsec_wrapper PRIVATE -Wl,-z,max-page-size=16384)
add_executable(native_host_check EXCLUDE_FROM_ALL tests/native_host_check.cpp)
target_link_libraries(native_host_check PRIVATE UnrealEngine3)
target_link_options(native_host_check PRIVATE -Wl,-z,max-page-size=16384)
add_executable(native_ui_control_check EXCLUDE_FROM_ALL tests/native_ui_control_check.cpp)
target_link_libraries(native_ui_control_check PRIVATE UnrealEngine3)
target_link_options(native_ui_control_check PRIVATE -Wl,-z,max-page-size=16384)
add_executable(native_gl_check EXCLUDE_FROM_ALL tests/native_gl_check.cpp)
target_link_libraries(native_gl_check PRIVATE UnrealEngine3 EGL GLESv3)
target_link_options(native_gl_check PRIVATE -Wl,-z,max-page-size=16384)
add_executable(native_shader_warmup_check EXCLUDE_FROM_ALL "${COMMON}/../tests/bl2_shader_warmup_test.cpp")
target_link_options(native_shader_warmup_check PRIVATE -Wl,-z,max-page-size=16384)
foreach(check native_vulkan_thunks_check native_vulkan_shader_pack_check native_vulkan_renderer_check native_vulkan_materials_check
    native_vulkan_gfx_capture_check native_vulkan_gfx_lifetime_check native_vulkan_depth_stencil_check
    native_vulkan_framebuffer_extent_check)
    add_executable(${check} EXCLUDE_FROM_ALL tests/${check}.cpp)
    target_link_libraries(${check} PRIVATE UnrealEngine3)
    target_compile_options(${check} PRIVATE -UNDEBUG)
    target_link_options(${check} PRIVATE -Wl,-z,max-page-size=16384)
endforeach()
foreach(check native_vulkan_materials_check native_vulkan_gfx_capture_check native_vulkan_gfx_lifetime_check native_vulkan_depth_stencil_check
    native_vulkan_framebuffer_extent_check)
    target_link_libraries(${check} PRIVATE GLESv3 EGL)
endforeach()

# Proposed CPU protocol executable, run separately from hardware rendering tests.
add_executable(native_vulkan_seed_check EXCLUDE_FROM_ALL tests/native_vulkan_seed_check.cpp src/native_vulkan_seed.cpp src/native_sha256.cpp)
target_compile_definitions(native_vulkan_seed_check PRIVATE BL2_NATIVE_ARM32=1)
target_include_directories(native_vulkan_seed_check PRIVATE src "${COMMON}")
target_compile_options(native_vulkan_seed_check PRIVATE -UNDEBUG -mfpu=neon)
target_link_options(native_vulkan_seed_check PRIVATE -Wl,-z,max-page-size=16384)
