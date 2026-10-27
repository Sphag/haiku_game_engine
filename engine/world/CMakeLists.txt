# World data on the CPU: heightfield terrain runtime (US-047) with stitched LOD tiles, world partition data (US-048) with grid cells,
# asynchronous streaming (US-049) with the cell lifecycle, the always-loaded layer, cell files, and the large-world origin (US-050).
# The module depends on engine_core for Result, GUIDs, and files, and on GLM. The contracts are in
# engine/world/include/engine/world/terrain.hpp, terrain_edit.hpp (US-071 brushes and the stroke document), partition.hpp, streaming.hpp, and
# origin.hpp.
add_library(engine_world STATIC
  src/terrain.cpp
  src/terrain_edit.cpp
  src/partition.cpp
  src/streaming.cpp
  src/origin.cpp
)
target_include_directories(engine_world PUBLIC include)
target_link_libraries(engine_world PUBLIC engine_core)
engine_configure_first_party(engine_world)
