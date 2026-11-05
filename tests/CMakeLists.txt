cmake_minimum_required(VERSION 3.20)
project(mhgu_vita_port_host_tests LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

enable_testing()
find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
add_test(NAME pixel_font_tools
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_pixel_fonts.py")
add_test(NAME vertex_rg32f_lowering
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_vertex_rg32f.py")
add_test(NAME frame_window_metrics
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_frame_window.py")
add_test(NAME engine_prepare_ab_metrics
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_engine_prepare_ab.py")
add_test(NAME gpu_wait_metrics
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_gpu_wait_metrics.py")
add_test(NAME frame_cost_metrics
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_frame_cost_metrics.py")
add_test(NAME game_cost_metrics
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_game_cost_metrics.py")
add_test(NAME texture_memory_metrics
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_texture_memory_metrics.py")
add_test(NAME resource_lifecycle_metrics
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_resource_lifecycle.py")

set(VP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set(VP_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${VP_SRC}/platform"
    "${VP_SRC}/loader"
    "${VP_SRC}/bridge"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs")

find_package(Threads)

# Compile exact production sections into a host test. Markers turn a renamed or
# removed section into a configure error instead of silently testing a copy.
function(vp_extract_marked source begin_marker end_marker out_var)
    string(FIND "${source}" "${begin_marker}" begin)
    if(begin EQUAL -1)
        message(FATAL_ERROR "Missing host-test source marker: ${begin_marker}")
    endif()
    string(FIND "${source}" "${end_marker}" end)
    if(end EQUAL -1 OR end LESS begin)
        message(FATAL_ERROR "Missing host-test source marker: ${end_marker}")
    endif()
    string(LENGTH "${end_marker}" end_length)
    math(EXPR section_length "${end} - ${begin} + ${end_length}")
    string(SUBSTRING "${source}" ${begin} ${section_length} section)
    set(${out_var} "${section}" PARENT_SCOPE)
endfunction()

file(READ "${VP_SRC}/platform/gxm_runtime.c" VP_GXM_RUNTIME_SOURCE)
set(VP_GXM_RENDER_PIPELINE_SOURCE "")
foreach(section
        pair_base pair_struct pair_hint color_mask pair_match pair_probe
        direct_refs notify pending_state pending_functions blend_ops blend_build
        pair_uniform)
    vp_extract_marked("${VP_GXM_RUNTIME_SOURCE}"
        "/* VP_HOST_TEST_EXTRACT:${section}_begin */"
        "/* VP_HOST_TEST_EXTRACT:${section}_end */"
        extracted)
    string(APPEND VP_GXM_RENDER_PIPELINE_SOURCE "${extracted}\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gxm_render_pipeline_production.inc"
     "${VP_GXM_RENDER_PIPELINE_SOURCE}")

# Mode-6 clear defer: admission, record, materialization and in-scene
# consumption are extracted from the same production file. The surface struct
# is a separate include so the host fixture can define its GXM handle stubs
# before referencing the type. Section order follows the definitions the
# remaining extracted bodies need.
vp_extract_marked("${VP_GXM_RUNTIME_SOURCE}"
    "/* VP_HOST_TEST_EXTRACT:clear_surface_begin */"
    "/* VP_HOST_TEST_EXTRACT:clear_surface_end */"
    vp_clear_surface_section)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/clear_surface_production.inc"
     "${vp_clear_surface_section}")

set(VP_CLEAR_DEFER_SOURCE "")
foreach(section
        clear_reason_enum clear_reason_name clear_reason_sync
        clear_defer_admit clear_defer_record clear_defer_submit
        clear_materialize clear_in_scene_consume)
    vp_extract_marked("${VP_GXM_RUNTIME_SOURCE}"
        "/* VP_HOST_TEST_EXTRACT:${section}_begin */"
        "/* VP_HOST_TEST_EXTRACT:${section}_end */"
        extracted)
    string(APPEND VP_CLEAR_DEFER_SOURCE "${extracted}\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/clear_defer_production.inc"
     "${VP_CLEAR_DEFER_SOURCE}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${VP_SRC}/platform/gxm_runtime.c")

# Upload-identity -> GXM format mapping: the host fixture supplies VitaSDK
# enum values and the extracted function is compiled unchanged.
vp_extract_marked("${VP_GXM_RUNTIME_SOURCE}"
    "/* VP_HOST_TEST_EXTRACT:frame_texture_format_begin */"
    "/* VP_HOST_TEST_EXTRACT:frame_texture_format_end */"
    vp_frame_texture_format_section)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/frame_texture_format_production.inc"
     "${vp_frame_texture_format_section}")

function(vp_host_test name)
    add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/${name}.c" ${ARGN})
    target_include_directories(${name} PRIVATE ${VP_INCLUDES})
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /UNDEBUG)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -UNDEBUG)
    endif()
    if(UNIX)
        target_link_libraries(${name} PRIVATE m)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

vp_host_test(audio_downmix)
vp_host_test(audio_ipc "${VP_SRC}/platform/audio_ipc.c")
vp_host_test(batch_policy)
vp_host_test(engine_submit_probe)
vp_host_test(engine_material "${VP_SRC}/platform/engine_material.c")
vp_host_test(engine_state "${VP_SRC}/platform/engine_state.c")
vp_host_test(engine_queue_native "${VP_SRC}/platform/engine_queue_native.c"
    "${VP_SRC}/platform/engine_state.c" "${VP_SRC}/platform/engine_material.c")
vp_host_test(engine_layout "${VP_SRC}/platform/engine_layout.c" "${VP_SRC}/platform/gpu_stream.c")
target_compile_definitions(engine_layout PRIVATE VP_ENGINE_LAYOUT_CORE_ONLY=1)
# audio_auto_catchup needs MAP_32BIT and is built in the UNIX block below.
vp_host_test(clear_defer)
target_include_directories(clear_defer PRIVATE
                           "${CMAKE_CURRENT_BINARY_DIR}")
vp_host_test(clear_fusion "${VP_SRC}/platform/gxm_depth.c")
vp_host_test(engine_mode)
vp_host_test(work_log)
vp_host_test(guest_heap_config)
vp_host_test(float_bits_numeric)
vp_host_test(frame_cost_profile "${VP_SRC}/platform/frame_cost_profile.c")
vp_host_test(gpu_command_buffer "${VP_SRC}/platform/gpu_stream.c")
vp_host_test(gpu_effective_rt "${VP_SRC}/platform/gpu_stream.c")
vp_host_test(gpu_indices)
vp_host_test(gpu_stream "${VP_SRC}/platform/gpu_stream.c")
vp_host_test(gpu_texture_decode "${VP_SRC}/platform/gpu_texture.c")
vp_host_test(pixel_font)
vp_host_test(gpu_uniform_state "${VP_SRC}/platform/gpu_stream.c")
vp_host_test(gpu_wait_profile)
vp_host_test(gxm_depth "${VP_SRC}/platform/gxm_depth.c")
vp_host_test(gxm_render_pipeline)
target_include_directories(gxm_render_pipeline PRIVATE
                           "${CMAKE_CURRENT_BINARY_DIR}")
vp_host_test(gxm_texture_format)
target_include_directories(gxm_texture_format PRIVATE
                           "${CMAKE_CURRENT_BINARY_DIR}")
vp_host_test(gxm_depth_view
    "${VP_SRC}/platform/gxm_depth_view.c"
    "${VP_SRC}/platform/gxm_resource_layout.c"
    "${VP_SRC}/platform/gxm_shadow_sampler.c"
    "${VP_SRC}/platform/static_shader_index.c")
vp_host_test(gxm_depth_view_shadow
    "${VP_SRC}/platform/gxm_depth_view.c"
    "${VP_SRC}/platform/gxm_resource_layout.c"
    "${VP_SRC}/platform/gxm_shadow_sampler.c"
    "${VP_SRC}/platform/static_shader_index.c")
vp_host_test(gxm_resource_layout
    "${VP_SRC}/platform/gxm_resource_layout.c"
    "${VP_SRC}/platform/static_shader_index.c")
vp_host_test(gxm_sampler_slots)
vp_host_test(gxm_sampler_plan)
vp_host_test(gxm_shader_plan)
vp_host_test(gxm_shadow_sampler "${VP_SRC}/platform/gxm_shadow_sampler.c")
vp_host_test(memory_attributes "${VP_SRC}/platform/memory_attributes.c")
vp_host_test(media_strided_copy)
vp_host_test(nvmap_table "${VP_SRC}/platform/nvmap_table.c")
vp_host_test(pair_cache_policy)
vp_host_test(render_resolution)
vp_host_test(static_shader_index "${VP_SRC}/platform/static_shader_index.c")
vp_host_test(texture_pool "${VP_SRC}/platform/texture_pool.c")
vp_host_test(texture_regions)
vp_host_test(texture_snapshot)
vp_host_test(time_calendar)
vp_host_test(vertex_format "${VP_SRC}/platform/vertex_format.c")
vp_host_test(vertex_window "${VP_SRC}/platform/vertex_window.c")

if(WIN32)
    vp_host_test(session_gate)
    set_tests_properties(session_gate PROPERTIES TIMEOUT 25)
    find_program(VP_TEST_POWERSHELL NAMES pwsh powershell REQUIRED)
    add_test(NAME runtime_main_window COMMAND "${VP_TEST_POWERSHELL}"
        -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/runtime_main_window.ps1")
endif()

# These two use POSIX-only memory and pthread facilities; CI runs them on Linux.
if(UNIX)
    vp_host_test(audio_auto_catchup "${VP_SRC}/platform/audio_ipc.c")
    vp_host_test(audio_wave_progress "${VP_SRC}/platform/audio_ipc.c")

    vp_host_test(guest_storage)

    if(Threads_FOUND)
        vp_host_test(log_sink "${VP_SRC}/platform/log_sink.c")
        target_link_libraries(log_sink PRIVATE Threads::Threads)
    endif()
endif()
