# Benchmark harness (US-074): the engine_benchmark library (manifest, measurement, performance contract, report) and the
# engine_bench command. The library is what the unit tests check; the command adds the device probe and the run loop.
# The build type is passed to the library so a report records whether it was measured in Release.
#
# The vendored nlohmann/json is a system include private to this library, so its warnings stay out of the first-party policy.
add_library(engine_benchmark STATIC
  src/manifest.cpp
  src/measure.cpp
  src/report.cpp
  src/cli.cpp
  src/workload.cpp
  src/physics_stress.cpp
  src/streaming_stress.cpp
)
target_include_directories(engine_benchmark PUBLIC include)
target_include_directories(engine_benchmark SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/single_include)
target_compile_definitions(engine_benchmark PUBLIC ENGINE_BENCH_BUILD_TYPE="$<CONFIG>")
target_link_libraries(engine_benchmark PUBLIC engine_core engine_assets)
# physics_stress (US-077) builds and steps a Jolt world headlessly; the physics and ECS headers stay out of the public interface.
target_link_libraries(engine_benchmark PRIVATE engine_physics engine_ecs engine_world)
# streaming_stress (US-078) drives the world streaming manager; its world types stay out of the public interface.
engine_configure_first_party(engine_benchmark)

add_executable(engine_bench src/main.cpp src/render_stress.cpp)
target_link_libraries(engine_bench PRIVATE engine_benchmark engine_platform engine_rhi engine_core engine_render engine_converter engine_scene)
target_compile_definitions(engine_bench PRIVATE ENGINE_BENCH_MANIFEST="${PROJECT_SOURCE_DIR}/benchmarks/manifest.json")
engine_configure_first_party(engine_bench)

# render_stress shaders (US-076): the scene entries the GPU run loads, compiled from the engine's own scene.hlsl. The outputs carry a
# bench_ prefix so they do not collide with the test shaders of the same entries, and they live in the shared shaders folder.
if(ENGINE_BUILD_SHADER_TOOLCHAIN)
  set(_bench_scene_hlsl "${PROJECT_SOURCE_DIR}/engine/render/shaders/scene.hlsl")
  engine_add_hlsl_shader(bench_scene_vertex SOURCE ${_bench_scene_hlsl} ENTRY VSMain PROFILE vs_6_0)
  engine_add_hlsl_shader(bench_scene_pixel SOURCE ${_bench_scene_hlsl} ENTRY PSMain PROFILE ps_6_0)
  engine_add_hlsl_shader(bench_scene_pixel_blend SOURCE ${_bench_scene_hlsl} ENTRY PSBlend PROFILE ps_6_0)
  engine_add_hlsl_shader(bench_scene_shadow_vertex SOURCE ${_bench_scene_hlsl} ENTRY VSShadow PROFILE vs_6_0)
  engine_add_hlsl_shader(bench_scene_shadow_pixel SOURCE ${_bench_scene_hlsl} ENTRY PSShadow PROFILE ps_6_0)
  engine_add_hlsl_shader(bench_scene_indirect_vertex SOURCE ${_bench_scene_hlsl} ENTRY VSIndirect PROFILE vs_6_0)
  engine_add_hlsl_shader(bench_scene_cull SOURCE ${_bench_scene_hlsl} ENTRY CSCull PROFILE cs_6_0)
  engine_add_hlsl_shader(bench_scene_meshlet_task SOURCE ${_bench_scene_hlsl} ENTRY ASMeshlet PROFILE as_6_5)
  engine_add_hlsl_shader(bench_scene_meshlet_mesh SOURCE ${_bench_scene_hlsl} ENTRY MSMeshlet PROFILE ms_6_5)
  engine_add_hlsl_shader(bench_scene_reflections SOURCE ${PROJECT_SOURCE_DIR}/engine/render/shaders/reflections.hlsl ENTRY CSReflections PROFILE cs_6_0)
  # Post pass (US-039): TAA, the two bloom blurs, and the tone map. Without them the native profile's TAA cannot run.
  set(_bench_post_hlsl "${PROJECT_SOURCE_DIR}/engine/render/shaders/postprocess.hlsl")
  engine_add_hlsl_shader(bench_post_taa SOURCE ${_bench_post_hlsl} ENTRY CSTaa PROFILE cs_6_0)
  engine_add_hlsl_shader(bench_post_bloom_horizontal SOURCE ${_bench_post_hlsl} ENTRY CSBloomHorizontal PROFILE cs_6_0)
  engine_add_hlsl_shader(bench_post_bloom_vertical SOURCE ${_bench_post_hlsl} ENTRY CSBloomVertical PROFILE cs_6_0)
  engine_add_hlsl_shader(bench_post_tonemap SOURCE ${_bench_post_hlsl} ENTRY CSTonemap PROFILE cs_6_0)
  add_dependencies(engine_bench
    engine_shader_bench_scene_vertex engine_shader_bench_scene_pixel engine_shader_bench_scene_pixel_blend
    engine_shader_bench_scene_shadow_vertex engine_shader_bench_scene_shadow_pixel engine_shader_bench_scene_indirect_vertex
    engine_shader_bench_scene_cull engine_shader_bench_scene_meshlet_task engine_shader_bench_scene_meshlet_mesh
    engine_shader_bench_scene_reflections engine_shader_bench_post_taa engine_shader_bench_post_bloom_horizontal
    engine_shader_bench_post_bloom_vertical engine_shader_bench_post_tonemap)
  target_compile_definitions(engine_bench PRIVATE ENGINE_BENCH_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders")
else()
  target_compile_definitions(engine_bench PRIVATE ENGINE_BENCH_SHADER_DIR="")
endif()

# engine_workload (US-075) generates the seeded scene workloads and checks them against benchmarks/workload_expected.json.
add_executable(engine_workload src/workload_main.cpp)
target_link_libraries(engine_workload PRIVATE engine_benchmark engine_core)
target_compile_definitions(engine_workload PRIVATE ENGINE_BENCH_MANIFEST="${PROJECT_SOURCE_DIR}/benchmarks/manifest.json")
engine_configure_first_party(engine_workload)
