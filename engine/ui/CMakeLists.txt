# Game UI (US-058): canvas layout with anchors and uniform scaling, images, UTF-8 text, buttons, sliders, focus and navigation, and
# the CPU rasterizer whose RGBA8 output the host composites after post-processing (PostProcessor::set_overlay). The library needs no
# editor or ImGui. Text rasterization goes through a C shim over the vendored stb_truetype, outside the first-party warning policy.
# docs/ui.md describes the contract.
add_library(engine_ui_native STATIC src/native/truetype_shim.c)
target_include_directories(engine_ui_native SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
target_include_directories(engine_ui_native PRIVATE src/native)
set_target_properties(engine_ui_native PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)

add_library(engine_ui STATIC
  src/geometry.cpp
  src/font.cpp
  src/surface.cpp
  src/ui.cpp
  src/ui_layout.cpp
)
target_include_directories(engine_ui PUBLIC include)
target_include_directories(engine_ui PRIVATE src src/native)
target_link_libraries(engine_ui PUBLIC engine_core engine_platform)
# nlohmann/json is a system include: its warnings stay out of the first-party -Werror policy. Only ui_layout.cpp includes it.
target_include_directories(engine_ui SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/single_include)
target_link_libraries(engine_ui PRIVATE engine_ui_native)
engine_configure_first_party(engine_ui)
