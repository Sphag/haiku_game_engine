# Render graph (US-032): pass declarations, compilation into an ordered plan with barriers and transient slots, execution through the
# RHI contract, and JSON export. The library is backend-agnostic: it talks to engine_rhi only, so the same graph runs on DX12, Vulkan,
# and the CPU reference device.
#
# Scene rendering (US-033): cameras and culling, mesh validation, render extraction into an immutable frame snapshot, a CPU reference
# rasterizer, and the scene renderer that draws a snapshot through the render graph. The extraction reads the scene graph, so this
# library links engine_scene. shaders/scene.hlsl is compiled by the test target, which loads the outputs at run time.
#
# Physically based shading (US-034): materials, instances, texture slots, image-based lighting, and the CPU reference of the scene shader.
#
# Direct lighting and shadows (US-035): light model, clusters, cascade fitting, and the shadow atlas. docs/lighting.md describes them.
#
# Skinning and motion vectors (US-036): skinned mesh data, palettes in the frame buffer, and the velocity target. docs/animation.md describes them.
#
# Mesh shaders (US-038): meshlet data of a cooked mesh, its validation against the source triangles, and the CPU reference of the task stage's
# culling. docs/mesh-shaders.md describes them.
#
# HDR postprocessing (US-039): jitter, exposure, bloom prefilter, tone mapping, and the TAA history. PostProcessor runs the same passes on the
# GPU (shaders/postprocess.hlsl). docs/postprocess.md describes them.
#
# Dynamic ray-traced GI (US-044): the probe-grid pass (shaders/dynamic_gi.hlsl) and its shared upload helper. docs/dynamic-gi.md describes it.
#
# Denoising and volumetrics (US-045): the temporal denoiser (shaders/denoise.hlsl) and the froxel fog (shaders/volumetric_fog.hlsl), with
# their CPU references. docs/denoise-volumetrics.md describes them.
#
# Native-profile GI and reflections (US-040): the baked probe grid is bound to the scene pass, the probe-coverage debug view, and the screen-space
# reflection pass (shaders/reflections.hlsl). It links engine_gi for the bake format and the trace settings. docs/gi-probes.md describes them.
#
# Temporal upscaling (US-046): the image profiles' fixed resolutions, the FSR 2 Quality size and jitter, and the upscaler's history and benchmark
# policy (upscaler.hpp). docs/upscaling.md describes them.
#
# GPU particles (US-051): emitters, curves, the budget, and the compute and billboard passes (shaders/particles.hlsl). docs/particles.md describes them.
add_library(engine_render STATIC
  src/render_graph.cpp
  src/render_graph_execute.cpp
  src/render_graph_export.cpp
  src/camera.cpp
  src/mesh.cpp
  src/terrain_mesh.cpp
  src/meshlet.cpp
  src/material.cpp
  src/pbr.cpp
  src/frame_snapshot.cpp
  src/scene_image.cpp
  src/reference_raster.cpp
  src/scene_renderer.cpp
  src/lighting.cpp
  src/shadow_atlas.cpp
  src/postprocess.cpp
  src/post_processor.cpp
  src/rt_effects.cpp
  src/gpu_upload.cpp
  src/dynamic_gi.cpp
  src/denoiser.cpp
  src/volumetric_fog.cpp
  src/upscaler.cpp
  src/particles.cpp
)
target_include_directories(engine_render PUBLIC include)
target_link_libraries(engine_render PUBLIC engine_core engine_rhi engine_scene engine_ecs engine_gi engine_world)
engine_configure_first_party(engine_render)
