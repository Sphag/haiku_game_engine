# Animation playback (US-036): skeletons, clips, pose sampling, two-clip blending, root motion, skinning palettes, and animated bounds.
# The state machine (US-069) adds the versioned state graph (state_graph.cpp) and its fixed-step runtime (state_machine.cpp).
# The editable graph document (US-070, graph_edit.cpp) keeps edits checked and undoable.
# The library is CPU-only and does not depend on the renderer: palettes are plain matrices that the scene renderer consumes.
#
# The vendored nlohmann/json is a system include private to this library, so its warnings stay out of the first-party -Werror policy.
add_library(engine_animation STATIC
  src/skeleton.cpp
  src/clip.cpp
  src/playback.cpp
  src/skinning.cpp
  src/state_graph.cpp
  src/state_machine.cpp
  src/graph_edit.cpp
)
target_include_directories(engine_animation PUBLIC include)
target_include_directories(engine_animation SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/single_include)
target_link_libraries(engine_animation PUBLIC engine_core)
engine_configure_first_party(engine_animation)
