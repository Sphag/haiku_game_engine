# Shader pipeline (US-031): HLSL compilation through the pinned DXC (compiler.cpp), reflection of DXIL and SPIR-V listings
# (reflection.cpp), the compiled-pipeline cache (pipeline_cache.cpp), and reloadable compute programs (shader_program.cpp).
# The DXC executable is a separate build product; this library only runs it, so its warnings never reach this target.
add_library(engine_shader STATIC
  src/compiler.cpp
  src/diagnostics.cpp
  src/pipeline_cache.cpp
  src/reflection.cpp
  src/shader_program.cpp
)
target_include_directories(engine_shader PUBLIC include)
target_link_libraries(engine_shader PUBLIC engine_core engine_assets engine_rhi)
engine_configure_first_party(engine_shader)
