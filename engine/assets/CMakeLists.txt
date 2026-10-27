# Asset registry (US-018): GUID-keyed records, sidecar metadata, and the project scan. The sidecar JSON is read with the
# vendored nlohmann/json as a system include, so its warnings stay out of the first-party -Werror policy.
#
# Runtime asset handles (US-024) and the cooked formats the runtime reads (the container, the texture payload) live here
# too. The runtime links this library but never the converter (FR-03).
add_library(engine_assets STATIC
  src/content_hash.cpp
  src/asset_registry.cpp
  src/asset_project.cpp
  src/cooked_container.cpp
  src/texture_format.cpp
)
target_include_directories(engine_assets PUBLIC include)
target_include_directories(engine_assets SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/single_include)
target_link_libraries(engine_assets PUBLIC engine_core)
engine_configure_first_party(engine_assets)
