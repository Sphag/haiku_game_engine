# Resource packs (US-060): the pack format, the builder that walks a dependency closure, and the verifying reader. The
# standalone runtime and the asset packer both link this library. It never links the converter (FR-03).
#
# The vendored nlohmann/json is a system include private to this library, so its warnings stay out of the first-party
# -Werror policy.
add_library(engine_pack STATIC
  src/pack.cpp
)
target_include_directories(engine_pack PUBLIC include)
target_include_directories(engine_pack SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/single_include)
target_link_libraries(engine_pack PUBLIC engine_core engine_assets)
engine_configure_first_party(engine_pack)
