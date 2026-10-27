# Game saves (US-059): slot files with a schema version, build metadata, and a checksum; the gameplay components a save selects; the
# Lua state of script instances (captured by engine_script); and the world cells changed since authoring. Files are JSON, read and
# written with the vendored nlohmann/json as a system include, so its warnings stay out of the first-party -Werror policy.
# The contract is in engine/save/include/engine/save/save.hpp.
add_library(engine_save STATIC
  src/save.cpp
  src/world_changes.cpp
)
target_include_directories(engine_save PUBLIC include)
target_include_directories(engine_save SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/single_include)
# The build version recorded in every save, from the project version.
target_compile_definitions(engine_save PRIVATE ENGINE_BUILD_VERSION="${PROJECT_VERSION}")
target_link_libraries(engine_save PUBLIC engine_core engine_scene engine_world)
engine_configure_first_party(engine_save)
