# Native-profile global illumination (US-040): baked diffuse irradiance probes and the CPU reference of screen-space reflections.
# Dynamic probe-grid bookkeeping (US-044): the camera-following cells, sample counts, and update plan of the ray-traced grid.
# The converter bakes the probes through this library, and the runtime reads them back. It depends on engine_core, engine_assets (the
# cooked container), and GLM, so the converter does not pull in the RHI. docs/gi-probes.md describes the format and the sampling.
add_library(engine_gi STATIC
  src/cooked_volume.cpp
  src/probes.cpp
  src/dynamic_grid.cpp
  src/screen_reflections.cpp
)
target_include_directories(engine_gi PUBLIC include)
target_link_libraries(engine_gi PUBLIC engine_core engine_assets)
engine_configure_first_party(engine_gi)
