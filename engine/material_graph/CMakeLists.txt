# Material graphs (US-067): the versioned graph and its validator (graph.cpp), the HLSL generator (codegen.cpp), the CPU reference of the
# generated surface (evaluate.cpp), and the per-variant compile through the pinned compiler and the pipeline cache (compile.cpp).
# The editable document (edit.cpp, US-068) holds the graph, its layout, selection, and undo history without any UI.
# docs/material-graph.md describes the format, the variants, and the cache keys.
#
# The vendored nlohmann/json is a system include private to this library, so its warnings stay out of the first-party -Werror policy.
add_library(engine_material_graph STATIC
  src/graph.cpp
  src/codegen.cpp
  src/evaluate.cpp
  src/compile.cpp
  src/edit.cpp
)
target_include_directories(engine_material_graph PUBLIC include)
target_include_directories(engine_material_graph SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/single_include)
target_link_libraries(engine_material_graph PUBLIC engine_core engine_assets engine_rhi engine_shader engine_render)
engine_configure_first_party(engine_material_graph)
