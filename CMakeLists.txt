cmake_minimum_required(VERSION 3.24)

# The VitaSDK toolchain is mandatory: this project always targets the Vita.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR
            "Set VITASDK to your VitaSDK root, or pass a VitaSDK toolchain file.")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE FILEPATH "VitaSDK toolchain")
endif()

project(mhgu_vita_port VERSION 0.2.0 LANGUAGES C)
if(NOT VITA)
    message(FATAL_ERROR "MHGU Vita Port requires the VitaSDK cross toolchain.")
endif()
include("${VITASDK}/share/vita.cmake" REQUIRED)

include(cmake/VitaPortOptions.cmake)

# ---------------------------------------------------------------------------
# Production runtime: native loader, platform services and GXM backend.
# ---------------------------------------------------------------------------
if(VITA_PORT_BUILD_NATIVE_LOADER)
include(cmake/GenerateInputHeaders.cmake)
enable_language(ASM)

add_executable(vita_port_native_loader
    src/app/main.c
    src/loader/native_arena.c
    src/loader/native_executor.c
    src/loader/native_lifecycle.c
    src/loader/native_link.c
    src/loader/native_startup.c
    src/loader/native_unwind.c
    src/loader/nso_loader.c
    src/loader/patch_loader.c
    src/loader/rtld_connections.c
    src/loader/sdk_dso.c
    src/bridge/guest_context.c
    src/bridge/host_bridge.c
    src/bridge/armv7/guest_tls.S
    src/bridge/armv7/guest_tls_island.S
    src/bridge/armv7/host_bridge.S
    src/bridge/armv7/loader_boundary.S
    src/bridge/armv7/sdk_dso.S
    src/bridge/armv7/sdk_svc_site.S
    src/bridge/armv7/sdk_timer_site.S
    src/bridge/armv7/udiv_island.S
    src/platform/audio_ipc.c
    src/platform/audio_render.c
    src/platform/audio_sink.c
    src/platform/guest_save.c
    src/platform/guest_storage.c
    src/platform/guest_svc.c
    src/platform/gpu_stream.c
    src/platform/gpu_texture.c
    src/platform/engine_layout.c
    src/platform/engine_material.c
    src/platform/engine_state.c
    src/platform/engine_queue_native.c
    src/platform/engine_full.c
    src/platform/gxm_depth.c
    src/platform/gxm_depth_clear_native.c
    src/platform/gxm_depth_view.c
    src/platform/gxm_depth_view_native.c
    src/platform/gxm_resource_layout.c
    src/platform/gxm_runtime.c
    src/platform/gxm_shadow_sampler.c
    src/platform/loader_exception_info.c
    src/platform/log_sink.c
    src/platform/media_decode.c
    src/platform/media_video.c
    src/platform/native_movie.c
    src/platform/memory_attributes.c
    src/platform/nvmap_table.c
    src/platform/sdk_exception_index.c
    src/platform/sdk_tls_slots.c
    src/platform/static_shader_index.c
    src/platform/thread_local_allocator.c
    src/platform/vertex_format.c
    src/platform/vertex_window.c)

set_property(SOURCE src/bridge/armv7/udiv_island.S APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/armv7/udiv_r3_r1_r2.inc.S")
set_property(SOURCE src/bridge/armv7/guest_tls_island.S APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/armv7/guest_tls.inc.S")
set_property(SOURCE src/bridge/armv7/host_bridge.S APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/armv7/guest_tls.inc.S")
set_property(SOURCE src/bridge/armv7/loader_boundary.S APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/armv7/host_bridge.inc.S")

target_include_directories(vita_port_native_loader PRIVATE
    src/app src/loader src/bridge src/bridge/armv7 src/platform src/generated
    "${VITA_PORT_GENERATED_DIR}")
target_compile_features(vita_port_native_loader PRIVATE c_std_11)
set_source_files_properties(src/platform/engine_material.c src/platform/engine_state.c
    src/platform/engine_queue_native.c PROPERTIES
    COMPILE_DEFINITIONS "memcpy=vp_engine_copy;memset=vp_engine_clear"
    COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns")
set_source_files_properties(src/platform/engine_full.c PROPERTIES
    COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns")
if(VITA_PORT_ENGINE_CAPTURE)
    target_sources(vita_port_native_loader PRIVATE src/platform/engine_capture.c)
    target_compile_definitions(vita_port_native_loader PRIVATE VP_ENGINE_CAPTURE=1)
    set_source_files_properties(src/platform/engine_capture.c PROPERTIES
        COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns")
endif()
target_compile_options(vita_port_native_loader PRIVATE
    -O2 -mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard
    -Wall -Wextra -Werror)
target_link_options(vita_port_native_loader PRIVATE
    -Wl,--no-undefined
    "LINKER:--defsym=vp_fixture_exidx_start=ADDR(.ARM.exidx)"
    "LINKER:--defsym=vp_fixture_exidx_end=ADDR(.ARM.exidx)+SIZEOF(.ARM.exidx)"
    "LINKER:--target2=got-rel"
    "LINKER:--defsym=__sce_headroom=0x10000")
target_link_libraries(vita_port_native_loader PRIVATE
    SceSysmem_stub SceIofilemgr_stub SceGxm_stub SceDisplay_stub
    SceCtrl_stub SceLibKernel_stub)
target_compile_definitions(vita_port_native_loader PRIVATE
    "VP_GXM_STATIC_DIR=\"${VITA_PORT_STATIC_SHADER_PACKAGE_DIR}\"")

# The original SDK DSO is executed natively with ABI-bound entry points. This is
# the supported production path; disabling it is a diagnostic-only build.
if(VITA_PORT_NATIVE_SDK_DSO)
    target_compile_definitions(vita_port_native_loader PRIVATE VP_NATIVE_SDK_DSO=1)

    # Guest-side ARM fixtures are compiled as freestanding objects and linked
    # into the loader; they are not host translation units.
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sdk_cpp_fixture.o"
        COMMAND "${VITASDK}/bin/arm-vita-eabi-g++" -c
            "${CMAKE_CURRENT_SOURCE_DIR}/src/loader/fixtures/sdk_cpp_fixture.cpp"
            -o "${CMAKE_CURRENT_BINARY_DIR}/sdk_cpp_fixture.o"
            -O2 -mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard
            -fexceptions -funwind-tables -fno-rtti
            -fno-reorder-blocks-and-partition -fno-toplevel-reorder
            -Wall -Wextra -Werror
        DEPENDS src/loader/fixtures/sdk_cpp_fixture.cpp VERBATIM)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/sdk_cpp_fixture.o"
        PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/audio_probe.o"
        COMMAND "${VITASDK}/bin/arm-vita-eabi-gcc" -c
            "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/audio_probe.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/audio_probe.o"
            -O2 -mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard
            -ffreestanding -fno-builtin -fno-stack-protector
            -Wall -Wextra -Werror
        DEPENDS src/platform/audio_probe.c src/platform/audio_probe.h VERBATIM)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/audio_probe.o"
        PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

    target_sources(vita_port_native_loader PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/sdk_cpp_fixture.o"
        "${CMAKE_CURRENT_BINARY_DIR}/audio_probe.o")
    target_link_libraries(vita_port_native_loader PRIVATE
        SceKernelThreadMgr_stub SceAudio_stub SceVideodec_stub SceSysmodule_stub)
    if(VITA_PORT_SAVE_SELFTEST)
        target_compile_definitions(vita_port_native_loader PRIVATE VP_SAVE_SELFTEST=1)
    endif()
endif()

if(VITA_PORT_KUBRIDGE_LIBRARY)
    if(NOT EXISTS "${VITA_PORT_KUBRIDGE_LIBRARY}")
        message(FATAL_ERROR "VITA_PORT_KUBRIDGE_LIBRARY does not exist: ${VITA_PORT_KUBRIDGE_LIBRARY}")
    endif()
    target_sources(vita_port_native_loader PRIVATE src/platform/native_kubridge.c)
    target_link_libraries(vita_port_native_loader PRIVATE "${VITA_PORT_KUBRIDGE_LIBRARY}")
    target_compile_definitions(vita_port_native_loader PRIVATE VP_HAVE_KUBRIDGE=1)
endif()

# The runtime validates this sidecar before installing a static shader bank.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/static-shader-package.json"
    "{\n  \"schema\": 1,\n  \"runtime_dir\": \"${VITA_PORT_STATIC_SHADER_PACKAGE_DIR}\",\n  \"sidecar_format\": \"VPSRES1\"\n}\n")

vita_create_self(vita_port_native_loader.self vita_port_native_loader UNSAFE)
vita_create_vpk(vita_port_native_loader.vpk ${VITA_PORT_NATIVE_TITLE_ID}
    vita_port_native_loader.self
    VERSION 00.02 NAME "MHGU Vita Port"
    FILE "${VITA_PORT_EXEFS}/rtld" "game/rtld"
         "${VITA_PORT_EXEFS}/main" "game/main"
         "${VITA_PORT_EXEFS}/subsdk0" "game/subsdk0"
         "${VITA_PORT_EXEFS}/sdk" "game/sdk")
endif()

# ---------------------------------------------------------------------------
# Minimal bootstrap probe (no game input). Useful to check a fresh VitaSDK.
# ---------------------------------------------------------------------------
if(VITA_PORT_BUILD_BOOTSTRAP)
    add_executable(vita_port_bootstrap src/bootstrap/main.c)
    target_compile_features(vita_port_bootstrap PRIVATE c_std_11)
    target_compile_options(vita_port_bootstrap PRIVATE
        -mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard
        -Wall -Wextra -Werror)
    target_link_options(vita_port_bootstrap PRIVATE
        "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/vita_port_bootstrap.map"
        -Wl,--no-undefined)
    target_link_libraries(vita_port_bootstrap PRIVATE SceIofilemgr_stub)
    vita_create_self(vita_port_bootstrap.self vita_port_bootstrap)
    vita_create_vpk(vita_port_bootstrap.vpk MHGUVP001 vita_port_bootstrap.self
        VERSION 00.02 NAME "MHGU Vita Port Bootstrap")
endif()
