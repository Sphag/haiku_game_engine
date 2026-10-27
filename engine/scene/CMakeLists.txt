# Component reflection (US-015): descriptors, validation, and the catalog. The scene format, hierarchy, and prefabs build on this.
# Scene graph and transforms (US-016): the hierarchy is stored in ECS components, so it links engine_ecs.
# Scene format and prefabs (US-017): JSON files read and written with the vendored nlohmann/json, a system include so its
# warnings stay out of the first-party -Werror policy. Only scene_format.cpp includes it.
add_library(engine_scene STATIC
  src/reflection.cpp
  src/transform.cpp
  src/scene_graph.cpp
  src/scene_format.cpp
  src/scene.cpp
)
target_include_directories(engine_scene PUBLIC include)
target_include_directories(engine_scene SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/single_include)
target_link_libraries(engine_scene PUBLIC engine_core engine_ecs)
engine_configure_first_party(engine_scene)
