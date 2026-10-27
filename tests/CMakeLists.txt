add_executable(engine_tests
  src/main.cpp
  src/bench_tests.cpp
  src/bench_workload_tests.cpp
  src/bench_physics_tests.cpp
  src/bench_streaming_tests.cpp
  src/all_features_tests.cpp
  src/fixture_provenance_tests.cpp
  src/cli_options.cpp
  src/cli_options_tests.cpp
  src/status_tests.cpp
  src/lifecycle_tests.cpp
  src/core_assert_tests.cpp
  src/core_clock_tests.cpp
  src/core_coordinates_tests.cpp
  src/core_file_io_tests.cpp
  src/core_guid_tests.cpp
  src/core_jobs_tests.cpp
  src/core_log_tests.cpp
  src/core_path_tests.cpp
  src/core_result_tests.cpp
  src/core_units_tests.cpp
  src/core_utf8_tests.cpp
  src/platform_window_tests.cpp
  src/platform_input_tests.cpp
  src/platform_replay_tests.cpp
  src/platform_evdev_tests.cpp
  src/input_action_tests.cpp
  src/input_profile_tests.cpp
  src/input_router_tests.cpp
  src/input_scenario_tests.cpp
  src/ecs_world_tests.cpp
  src/ecs_storage_tests.cpp
  src/ecs_command_buffer_tests.cpp
  src/ecs_systems_tests.cpp
  src/scene_reflection_tests.cpp
  src/scene_transform_tests.cpp
  src/scene_graph_tests.cpp
  src/scene_format_tests.cpp
  src/scene_tests.cpp
  src/assets_registry_tests.cpp
  src/assets_project_tests.cpp
  src/assets_cooked_tests.cpp
  src/assets_handle_tests.cpp
  src/rhi_contract_tests.cpp
  src/rhi_upscaler_tests.cpp
  src/rhi_dx12_gpu_tests.cpp
  src/rhi_vulkan_gpu_tests.cpp
  src/rhi_vulkan_mapping_tests.cpp
  src/converter_tests.cpp
  src/converter_model_tests.cpp
  src/converter_mesh_tests.cpp
  src/converter_texture_tests.cpp
  src/converter_watch_tests.cpp
  src/shader_diagnostics_tests.cpp
  src/shader_reflection_tests.cpp
  src/shader_cache_tests.cpp
  src/render_graph_tests.cpp
  src/render_graph_gpu_tests.cpp
  src/render_scene_tests.cpp
  src/render_scene_gpu_tests.cpp
  src/render_material_tests.cpp
  src/material_graph_tests.cpp
  src/material_graph_edit_tests.cpp
  src/render_material_gpu_tests.cpp
  src/lighting_tests.cpp
  src/postprocess_tests.cpp
  src/lighting_gpu_tests.cpp
  src/animation_tests.cpp
  src/animation_state_machine_tests.cpp
  src/animation_graph_edit_tests.cpp
  src/render_skin_tests.cpp
  src/render_skin_gpu_tests.cpp
  src/render_indirect_gpu_tests.cpp
  src/render_meshlet_tests.cpp
  src/render_terrain_tests.cpp
  src/render_terrain_gpu_tests.cpp
  src/render_meshlet_gpu_tests.cpp
  src/render_postprocess_gpu_tests.cpp
  src/render_meshlet_fixture.hpp
  src/gi_probe_tests.cpp
  src/gi_reflection_tests.cpp
  src/render_gi_gpu_tests.cpp
  src/render_rt_fixture.hpp
  src/render_rt_tests.cpp
  src/render_rt_gpu_tests.cpp
  src/render_rt_scene_gpu_tests.cpp
  src/gi_dynamic_grid_tests.cpp
  src/render_dynamic_gi_tests.cpp
  src/render_dynamic_gi_gpu_tests.cpp
  src/render_denoise_tests.cpp
  src/render_denoise_gpu_tests.cpp
  src/render_volumetric_tests.cpp
  src/render_volumetric_gpu_tests.cpp
  src/render_particles_tests.cpp
  src/render_particles_gpu_tests.cpp
  src/render_upscaler_tests.cpp
  src/world_terrain_tests.cpp
  src/world_terrain_edit_tests.cpp
  src/world_partition_tests.cpp
  src/world_streaming_tests.cpp
  src/world_origin_tests.cpp
  src/physics_world_tests.cpp
  src/physics_query_tests.cpp
  src/physics_character_tests.cpp
  src/script_lua_tests.cpp
  src/script_lifecycle_tests.cpp
  src/script_audio_tests.cpp
  src/script_animation_tests.cpp
  src/script_ui_tests.cpp
  src/ui_layout_tests.cpp
  src/ui_text_tests.cpp
  src/ui_interaction_tests.cpp
  src/audio_decode_tests.cpp
  src/audio_mixer_tests.cpp
  src/audio_device_tests.cpp
  src/converter_world_tests.cpp
  src/save_tests.cpp
  src/pack_tests.cpp
  src/runtime_options_tests.cpp
  src/editor_fixture.hpp
  src/editor_project_tests.cpp
  src/editor_console_tests.cpp
  src/editor_workspace_tests.cpp
  src/editor_model_tests.cpp
  src/editor_scene_tests.cpp
  src/editor_viewport_tests.cpp
  src/editor_play_tests.cpp
  src/editor_terrain_tests.cpp
  src/editor_world_tests.cpp
  src/editor_distribution_tests.cpp
  src/all_features_editor_tests.cpp
  src/editor_asset_tests.cpp
  src/editor_imgui_tests.cpp
  src/editor_renderer_gpu_tests.cpp
  src/showcase_tests.cpp
)
# The Vulkan mapping tests include the backend's private headers and the vendored Vulkan-Headers, which stay system includes.
target_include_directories(engine_tests PRIVATE ../engine/rhi/src)
target_include_directories(engine_tests SYSTEM PRIVATE ../third_party/vulkan_headers/include)
# Win32 windows need a desktop session; their tests exist only where the Win32 backend does.
if(WIN32)
  target_sources(engine_tests PRIVATE src/platform_win32_tests.cpp src/platform_win32_input_tests.cpp)
  # The Win32 input tests decode raw mouse reports through the backend's helper header.
  target_include_directories(engine_tests PRIVATE ../engine/platform/src/win32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # X11 tests drive a real X server through the backend's Xlib ABI header and bind a few extra Xlib functions.
  target_sources(engine_tests PRIVATE src/platform_x11_tests.cpp src/platform_x11_input_tests.cpp)
  target_include_directories(engine_tests PRIVATE ../engine/platform/src/x11)
  target_link_libraries(engine_tests PRIVATE ${CMAKE_DL_LIBS})
endif()
target_link_libraries(engine_tests PRIVATE engine_benchmark engine_material_graph engine_gameplay engine_editor engine_runtime engine_platform engine_ecs engine_scene engine_assets engine_converter engine_shader engine_render engine_animation engine_gi engine_physics engine_script engine_audio engine_ui engine_world engine_save engine_pack engine_rhi engine_core Catch2::Catch2)
engine_configure_first_party(engine_tests)
# Checked-in fixtures (Cyrillic file names and contents) for the file IO tests and the converter project.
# Build Game tests package the standalone runtime, so they need it built first.
add_dependencies(engine_tests game_runtime)
target_compile_definitions(engine_tests PRIVATE ENGINE_TEST_GAME_RUNTIME="$<TARGET_FILE:game_runtime>" ENGINE_TEST_PACK_PROJECT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pack/project" ENGINE_SHOWCASE_DIR="${PROJECT_SOURCE_DIR}/samples/showcase")
target_compile_definitions(engine_tests PRIVATE ENGINE_TEST_FONT_PATH="${PROJECT_SOURCE_DIR}/third_party/roboto/src/hinted/Roboto-Regular.ttf")
target_compile_definitions(engine_tests PRIVATE ENGINE_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_compile_definitions(engine_tests PRIVATE ENGINE_TEST_BENCH_MANIFEST="${PROJECT_SOURCE_DIR}/benchmarks/manifest.json")
target_compile_definitions(engine_tests PRIVATE ENGINE_TEST_WORKLOAD_EXPECTED="${PROJECT_SOURCE_DIR}/benchmarks/workload_expected.json")

add_test(NAME engine_tests_unit COMMAND engine_tests --suite unit)
set_tests_properties(engine_tests_unit PROPERTIES LABELS unit)

add_test(NAME engine_tests_integration COMMAND engine_tests --suite integration)
set_tests_properties(engine_tests_integration PROPERTIES LABELS integration)

# CLI contract tests run the built executable and check its exit code and message.
# Arguments are passed as one '|'-separated string so each case stays a single CTest argument.
function(engine_add_cli_test name cli_args expected_exit expected_text)
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND}
      "-DEXECUTABLE=$<TARGET_FILE:engine_tests>"
      "-DCLI_ARGS=${cli_args}"
      "-DEXPECT_EXIT=${expected_exit}"
      "-DEXPECT_TEXT=${expected_text}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cli/CliContract.cmake")
  set_tests_properties(${name} PROPERTIES LABELS integration)
endfunction()

engine_add_cli_test(engine_tests_cli_help "--help" 0 "Usage: engine_tests")
engine_add_cli_test(engine_tests_cli_help_short "-h" 0 "Usage: engine_tests")
engine_add_cli_test(engine_tests_cli_unknown_argument "--bogus" 2 "unknown argument: --bogus")
engine_add_cli_test(engine_tests_cli_missing_suite_value "--suite" 2 "--suite requires a value")
engine_add_cli_test(engine_tests_cli_invalid_suite "--suite|bogus" 2 "invalid suite 'bogus'")
engine_add_cli_test(engine_tests_cli_gpu_needs_backend "--suite|gpu" 2 "--suite gpu requires --backend")
engine_add_cli_test(engine_tests_cli_invalid_backend "--suite|gpu|--backend|metal" 2 "invalid backend 'metal'")
# DX12 exists only on Windows. Elsewhere its GPU suite reports unsupported with exit 3.
if(NOT WIN32)
  engine_add_cli_test(engine_tests_cli_gpu_dx12_unsupported "--suite|gpu|--backend|dx12" 3 "exists only on Windows")
endif()
engine_add_cli_test(engine_tests_cli_suite_unit "--suite|unit" 0 "All tests passed")

# Asset converter CLI (US-019): the built tool's exit codes and its report on stdout ("-"). The fixture project is read-only;
# cooking writes into the build tree.
function(engine_add_converter_cli_test name cli_args expected_exit expected_text)
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND}
      "-DEXECUTABLE=$<TARGET_FILE:asset_converter>"
      "-DCLI_ARGS=${cli_args}"
      "-DEXPECT_EXIT=${expected_exit}"
      "-DEXPECT_TEXT=${expected_text}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cli/CliContract.cmake")
  set_tests_properties(${name} PROPERTIES LABELS integration)
endfunction()

set(ENGINE_CONVERTER_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/converter/project")
set(ENGINE_CONVERTER_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/converter_cli_out")
engine_add_converter_cli_test(engine_converter_cli_help "--help" 0 "Usage: asset_converter")
engine_add_converter_cli_test(engine_converter_cli_unknown_command "bogus" 2 "unknown command 'bogus'")
engine_add_converter_cli_test(engine_converter_cli_missing_project "validate" 2 "--project is required")
engine_add_converter_cli_test(engine_converter_cli_cook_needs_output "cook|--project|${ENGINE_CONVERTER_FIXTURE}" 2
  "cook needs an output directory")
engine_add_converter_cli_test(engine_converter_cli_validate_fixture
  "validate|--project|${ENGINE_CONVERTER_FIXTURE}|--report|-" 0 "\"status\": \"ok\"")
engine_add_converter_cli_test(engine_converter_cli_cook_fixture
  "cook|--project|${ENGINE_CONVERTER_FIXTURE}|--output|${ENGINE_CONVERTER_OUTPUT}|--report|-" 0 "\"status\": \"ok\"")
engine_add_converter_cli_test(engine_converter_cli_watch_needs_output "watch|--project|${ENGINE_CONVERTER_FIXTURE}" 2
  "watch needs an output directory")
engine_add_converter_cli_test(engine_converter_cli_watch_bounded_passes
  "watch|--project|${ENGINE_CONVERTER_FIXTURE}|--output|${ENGINE_CONVERTER_OUTPUT}|--max-passes|1|--report|-" 0 "\"status\": \"ok\"")
engine_add_converter_cli_test(engine_converter_cli_missing_project_directory
  "validate|--project|${CMAKE_CURRENT_BINARY_DIR}/no_such_project|--report|-" 1 "SCAN_IO")

# Shader pipeline (US-031). Cache files and scratch outputs are written under the build tree; the source directory holds the HLSL
# fixtures that the compiler tests read.
target_compile_definitions(engine_tests PRIVATE
  ENGINE_TEST_SHADER_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/shader_scratch"
  ENGINE_TEST_SHADER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shader_scratch")

# Compiled shader outputs that GPU tests load at run time (US-029).
target_compile_definitions(engine_tests PRIVATE ENGINE_TEST_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders")

# Shader toolchain (US-003): the pinned dxc compiles the HLSL fixture to DXIL and SPIR-V
# during the build; the tests check those outputs and the diagnostic format.
if(ENGINE_BUILD_SHADER_TOOLCHAIN)
  engine_add_hlsl_shader(fixture SOURCE shaders/fixture.hlsl ENTRY main PROFILE cs_6_0)
  # Editor UI (US-061): the ImGui shaders of engine/editor, compiled by engine_editor, loaded by editor_renderer_gpu_tests.cpp.
  add_dependencies(engine_tests engine_shader_editor_imgui_vertex engine_shader_editor_imgui_pixel)
  # Resource fixture (US-029, US-030): a compute pass over byte-address buffers. rhi_dx12_gpu_tests.cpp loads the DXIL and
  # rhi_vulkan_gpu_tests.cpp loads the SPIR-V that dxc writes from the same source.
  engine_add_hlsl_shader(dx12_compute SOURCE shaders/dx12_compute.hlsl ENTRY main PROFILE cs_6_0)
  add_dependencies(engine_tests engine_shader_dx12_compute)
  # Ray tracing fixture (US-041, US-042): the hit/miss library. DX12 loads the DXIL and Vulkan loads the SPIR-V library from the same source.
  engine_add_hlsl_shader(dx12_raytrace SOURCE shaders/dx12_raytrace.hlsl PROFILE lib_6_3 DXIL_LIBRARY)
  add_dependencies(engine_tests engine_shader_dx12_raytrace)
  # Scene shader (US-033, PBR in US-034): the vertex, opaque pixel, and blended pixel entries of engine/render/shaders/scene.hlsl, loaded by the scene and material GPU tests.
  engine_add_hlsl_shader(scene_vertex SOURCE ../engine/render/shaders/scene.hlsl ENTRY VSMain PROFILE vs_6_0)
  engine_add_hlsl_shader(scene_pixel SOURCE ../engine/render/shaders/scene.hlsl ENTRY PSMain PROFILE ps_6_0)
  engine_add_hlsl_shader(scene_pixel_blend SOURCE ../engine/render/shaders/scene.hlsl ENTRY PSBlend PROFILE ps_6_0)
  # Shadow tile entries (US-035): the caster vertex stage and the depth pixel stage of the same file.
  engine_add_hlsl_shader(scene_shadow_vertex SOURCE ../engine/render/shaders/scene.hlsl ENTRY VSShadow PROFILE vs_6_0)
  engine_add_hlsl_shader(scene_shadow_pixel SOURCE ../engine/render/shaders/scene.hlsl ENTRY PSShadow PROFILE ps_6_0)
  # Skinned vertex entries (US-036): the skinned scene and shadow vertex stages, which reuse the pixel entries above.
  engine_add_hlsl_shader(scene_skinned_vertex SOURCE ../engine/render/shaders/scene.hlsl ENTRY VSSkinned PROFILE vs_6_0)
  engine_add_hlsl_shader(scene_skinned_shadow_vertex SOURCE ../engine/render/shaders/scene.hlsl ENTRY VSSkinnedShadow PROFILE vs_6_0)
  # Terrain entries (US-047): the tile's vertex stage over the terrain stride (the splat at TEXCOORD3) and its shadow caster.
  engine_add_hlsl_shader(scene_terrain_vertex SOURCE ../engine/render/shaders/scene.hlsl ENTRY VSTerrain PROFILE vs_6_0)
  engine_add_hlsl_shader(scene_terrain_shadow_vertex SOURCE ../engine/render/shaders/scene.hlsl ENTRY VSTerrainShadow PROFILE vs_6_0)
  # Indirect entries (US-037): the indirect vertex stages and the GPU culling compute entry of the same file.
  engine_add_hlsl_shader(scene_indirect_vertex SOURCE ../engine/render/shaders/scene.hlsl ENTRY VSIndirect PROFILE vs_6_0)
  engine_add_hlsl_shader(scene_skinned_indirect_vertex SOURCE ../engine/render/shaders/scene.hlsl ENTRY VSSkinnedIndirect PROFILE vs_6_0)
  engine_add_hlsl_shader(scene_cull SOURCE ../engine/render/shaders/scene.hlsl ENTRY CSCull PROFILE cs_6_0)
  # Mesh shader entries (US-038): the culling amplification (task) stage and the mesh stage of the same file. Shader model 6.5 is the first
  # that carries them; the pixel stage is scene_pixel.
  engine_add_hlsl_shader(scene_meshlet_task SOURCE ../engine/render/shaders/scene.hlsl ENTRY ASMeshlet PROFILE as_6_5)
  engine_add_hlsl_shader(scene_meshlet_mesh SOURCE ../engine/render/shaders/scene.hlsl ENTRY MSMeshlet PROFILE ms_6_5)
  # Post-process entries (US-039): the TAA resolve, the two bloom blur passes, and the tone-map composite of postprocess.hlsl. They run through
  # the render graph as compute passes; the GPU suite loads them through PostProcessor.
  engine_add_hlsl_shader(post_taa SOURCE ../engine/render/shaders/postprocess.hlsl ENTRY CSTaa PROFILE cs_6_0)
  engine_add_hlsl_shader(post_bloom_horizontal SOURCE ../engine/render/shaders/postprocess.hlsl ENTRY CSBloomHorizontal PROFILE cs_6_0)
  engine_add_hlsl_shader(post_bloom_vertical SOURCE ../engine/render/shaders/postprocess.hlsl ENTRY CSBloomVertical PROFILE cs_6_0)
  engine_add_hlsl_shader(post_tonemap SOURCE ../engine/render/shaders/postprocess.hlsl ENTRY CSTonemap PROFILE cs_6_0)
  # Screen-space reflections (US-040): the compute pass of reflections.hlsl, which the native-profile GPU suite loads through SceneRenderer.
  engine_add_hlsl_shader(scene_reflections SOURCE ../engine/render/shaders/reflections.hlsl ENTRY CSReflections PROFILE cs_6_0)
  # Ray-traced shadows and reflections (US-043): inline ray queries, so the pass is a compute shader at SM 6.5 (RayQuery).
  engine_add_hlsl_shader(rt_effects SOURCE ../engine/render/shaders/rt_effects.hlsl ENTRY CSRayTracedEffects PROFILE cs_6_5 RAY_QUERY)
  # Dynamic GI (US-044): the probe-grid trace and blend, with inline ray queries against the same scene.
  engine_add_hlsl_shader(dynamic_gi SOURCE ../engine/render/shaders/dynamic_gi.hlsl ENTRY CSDynamicGi PROFILE cs_6_5 RAY_QUERY)
  # Denoising and volumetrics (US-045): the temporal denoiser, and the two fog passes of volumetric_fog.hlsl. The fog inject traces its shadows
  # with inline ray queries, and both fog entries share the file's acceleration-structure declaration, so both compile at SM 6.5.
  engine_add_hlsl_shader(denoise_temporal SOURCE ../engine/render/shaders/denoise.hlsl ENTRY CSTemporalDenoise PROFILE cs_6_0)
  engine_add_hlsl_shader(volumetric_fog_inject SOURCE ../engine/render/shaders/volumetric_fog.hlsl ENTRY CSFogInject PROFILE cs_6_5 RAY_QUERY)
  engine_add_hlsl_shader(volumetric_fog_integrate SOURCE ../engine/render/shaders/volumetric_fog.hlsl ENTRY CSFogIntegrate PROFILE cs_6_5 RAY_QUERY)
  engine_add_hlsl_shader(volumetric_fog_apply SOURCE ../engine/render/shaders/volumetric_fog.hlsl ENTRY CSFogApply PROFILE cs_6_5 RAY_QUERY)
  # GPU particles (US-051): the simulate and indirect-argument compute entries, and the billboard vertex and pixel stages of particles.hlsl.
  engine_add_hlsl_shader(particles_simulate SOURCE ../engine/render/shaders/particles.hlsl ENTRY CSParticleSimulate PROFILE cs_6_0)
  engine_add_hlsl_shader(particles_args SOURCE ../engine/render/shaders/particles.hlsl ENTRY CSParticleArgs PROFILE cs_6_0)
  engine_add_hlsl_shader(particles_vertex SOURCE ../engine/render/shaders/particles.hlsl ENTRY VSParticle PROFILE vs_6_0)
  engine_add_hlsl_shader(particles_pixel SOURCE ../engine/render/shaders/particles.hlsl ENTRY PSParticle PROFILE ps_6_0)
  add_dependencies(engine_tests engine_shader_scene_vertex engine_shader_scene_pixel engine_shader_scene_pixel_blend
    engine_shader_scene_shadow_vertex engine_shader_scene_shadow_pixel engine_shader_scene_skinned_vertex engine_shader_scene_skinned_shadow_vertex
    engine_shader_scene_indirect_vertex engine_shader_scene_skinned_indirect_vertex engine_shader_scene_cull
    engine_shader_scene_terrain_vertex engine_shader_scene_terrain_shadow_vertex
    engine_shader_scene_meshlet_task engine_shader_scene_meshlet_mesh
    engine_shader_post_taa engine_shader_post_bloom_horizontal engine_shader_post_bloom_vertical engine_shader_post_tonemap
    engine_shader_scene_reflections engine_shader_rt_effects engine_shader_dynamic_gi
    engine_shader_denoise_temporal engine_shader_volumetric_fog_inject engine_shader_volumetric_fog_integrate engine_shader_volumetric_fog_apply
    engine_shader_particles_simulate engine_shader_particles_args engine_shader_particles_vertex engine_shader_particles_pixel)

  # The shader pipeline tests run the pinned compiler itself, so they exist only when the toolchain is built.
  target_sources(engine_tests PRIVATE src/shader_compiler_tests.cpp src/shader_program_tests.cpp src/shader_gpu_tests.cpp src/material_graph_compile_tests.cpp)
  target_compile_definitions(engine_tests PRIVATE ENGINE_TEST_DXC_PATH="${ENGINE_DXC_EXECUTABLE}")

  add_test(NAME engine_shader_fixture_outputs
    COMMAND ${CMAKE_COMMAND}
      -DMODE=outputs
      "-DDXIL=${CMAKE_BINARY_DIR}/shaders/fixture.dxil"
      "-DSPIRV=${CMAKE_BINARY_DIR}/shaders/fixture.spv"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ShaderFixtureTests.cmake")
  set_tests_properties(engine_shader_fixture_outputs PROPERTIES LABELS integration)

  add_test(NAME engine_shader_diagnostic_path_and_line
    COMMAND ${CMAKE_COMMAND}
      -DMODE=diagnostic
      "-DDXC=${ENGINE_DXC_EXECUTABLE}"
      "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/shaders/fixture_error.hlsl"
      -DEXPECT_LINE=7
      "-DSCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/shader_diagnostics"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ShaderFixtureTests.cmake")
  set_tests_properties(engine_shader_diagnostic_path_and_line PROPERTIES LABELS integration)
endif()

# Resource packs and the standalone runtime (US-060). The pack CLI and the runtime CLI are checked by exit code and message; the
# round trip cooks and packs a fixture, then starts the game from an empty working directory.
function(engine_add_pack_cli_test name tool_target cli_args expected_exit expected_text)
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND}
      "-DEXECUTABLE=$<TARGET_FILE:${tool_target}>"
      "-DCLI_ARGS=${cli_args}"
      "-DEXPECT_EXIT=${expected_exit}"
      "-DEXPECT_TEXT=${expected_text}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cli/CliContract.cmake")
  set_tests_properties(${name} PROPERTIES LABELS integration)
endfunction()

set(ENGINE_PACK_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pack/project")
set(ENGINE_PACK_MISSING_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pack/missing_dependency/project")
set(ENGINE_PACK_PALETTE_GUID "7c234218-2288-45ea-a06d-833e300036a3")
set(ENGINE_PACK_START_SCENE "3898c084-2f9e-4e6f-af6c-4ae6e8920395")

engine_add_pack_cli_test(engine_pack_cli_help asset_packer "--help" 0 "Usage: asset_packer")
engine_add_pack_cli_test(engine_pack_cli_missing_root asset_packer
  "--project|${ENGINE_PACK_FIXTURE}|--cooked|${CMAKE_CURRENT_BINARY_DIR}/pack_none|--output|${CMAKE_CURRENT_BINARY_DIR}/pack_none.pack" 2
  "at least one --root is required")
engine_add_pack_cli_test(engine_pack_cli_missing_dependency_aborts asset_packer
  "--project|${ENGINE_PACK_MISSING_FIXTURE}|--cooked|${CMAKE_CURRENT_BINARY_DIR}/pack_none|--root|${ENGINE_PACK_START_SCENE}|--output|${CMAKE_CURRENT_BINARY_DIR}/pack_none.pack" 1
  "MissingDependency")
engine_add_pack_cli_test(engine_runtime_cli_help game_runtime "--help" 0 "Usage: game_runtime")
engine_add_pack_cli_test(engine_runtime_cli_missing_pack game_runtime "--start-scene|${ENGINE_PACK_START_SCENE}" 2 "--pack is required")
engine_add_pack_cli_test(engine_runtime_cli_invalid_backend game_runtime
  "--pack|a.pack|--start-scene|${ENGINE_PACK_START_SCENE}|--backend|metal" 2 "invalid backend 'metal'")
engine_add_pack_cli_test(engine_runtime_cli_missing_pack_file game_runtime
  "--pack|${CMAKE_CURRENT_BINARY_DIR}/no_such.pack|--start-scene|${ENGINE_PACK_START_SCENE}" 1 "Io: NotFound")

add_test(NAME engine_pack_runtime_round_trip
  COMMAND ${CMAKE_COMMAND}
    "-DCONVERTER=$<TARGET_FILE:asset_converter>"
    "-DPACKER=$<TARGET_FILE:asset_packer>"
    "-DRUNTIME=$<TARGET_FILE:game_runtime>"
    "-DPROJECT=${ENGINE_PACK_FIXTURE}"
    "-DSTART_SCENE=${ENGINE_PACK_START_SCENE}"
    "-DPALETTE_GUID=${ENGINE_PACK_PALETTE_GUID}"
    "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/pack_round_trip"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/pack/PackRuntimeRoundTrip.cmake")
set_tests_properties(engine_pack_runtime_round_trip PROPERTIES LABELS integration)
