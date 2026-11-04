# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/fibers.cpp
    common/host_memory.cpp
    common/host_memory_fallback.cpp
    common/param_package.cpp
    common/range_map.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/undefined_fix.cpp
    common/unique_function.cpp
    core/core_timing.cpp
    core/savestate.cpp
    core/savestate_kernel_handles.cpp
    core/savestate_service_sessions.cpp
    core/address_space.cpp
    core/page_heap.cpp
    core/read_traps.cpp
    core/nso.cpp
    core/nso_compatibility.cpp
    core/nrr.cpp
    core/loose_dlc.cpp
    core/loose_updates.cpp
    core/nup_writer.cpp
    core/joycon2_standin.cpp
    core/opus_parameters.cpp
    core/swkbd_config.cpp
    core/romfs_program_index.cpp
    core/system_archive.cpp
    core/ncz_decompressor.cpp
    core/opus_host_state.cpp
    core/internal_network/network.cpp
    video_core/memory_tracker.cpp
    video_core/transform_feedback.cpp
    video_core/memory_budget.cpp
    video_core/astc_recompression_override.cpp
    video_core/nx2_video_configuration.cpp
    video_core/nx2_decoder_image.cpp
    video_core/nx2_present.cpp
    video_core/nx2_gpu_mappings.cpp
    video_core/nx2_sparse_mappings.cpp
    video_core/nx2_sparse_region.cpp
    video_core/nx2_command_sync.cpp
    video_core/draw_instances.cpp
    video_core/native_qmd.cpp
    video_core/syncpoint_bounds.cpp
    video_core/surface_format.cpp
    video_core/display_timing.cpp
    video_core/physical_alias.cpp
    video_core/ampere_returns.cpp
    video_core/ampere_sured.cpp
    video_core/blit_dither.cpp
    video_core/ampere_bindless_surfaces.cpp
    audio_core/batch_decoder.cpp
    audio_core/hardware_audio_codec.cpp
    core/system_test_context.h
    video_core/brx_probe.cpp
    video_core/ampere_fefw.cpp
    video_core/ampere_fe_pipeline.cpp
    video_core/gpu_split_wait.cpp
    video_core/uniform_cache_history.cpp
    video_core/multi_range_refresh.cpp
    video_core/native_root_tables.cpp
    video_core/ampere_matrix.cpp
    video_core/spirv_software_matrix.cpp
    video_core/ampere_raiders.cpp
    video_core/shader_surface_store.cpp
    video_core/image_overlap.cpp
    video_core/pending_download.cpp
    video_core/ampere_f2i.cpp
    video_core/compute_storage_buffers.cpp
    video_core/ampere_kirby.cpp
    video_core/ampere_geometry.cpp
    video_core/mesh_shaders.cpp
    video_core/ampere_sports_resort.cpp
    video_core/nx2_rt_report.cpp
    video_core/descriptor_table_growth.cpp
    video_core/fire_emblem_upscale.cpp
    video_core/ampere_geometry_lowering.cpp
    video_core/ampere_geometry_exit.cpp
    video_core/ampere_uniform_lea.cpp
    video_core/ampere_bindless_tex.cpp
    video_core/ampere_yoshi.cpp
    video_core/ampere_release_guards.cpp
    video_core/ampere_release_f2i.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

if (WIN32)
    target_sources(tests PRIVATE common/host_memory_commit.cpp)
endif()

target_link_libraries(tests PRIVATE common core input_common video_core audio_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

add_test(NAME tests COMMAND tests)

# needed for vma
if (NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()
