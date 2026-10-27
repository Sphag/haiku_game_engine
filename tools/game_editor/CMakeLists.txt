# Game editor (US-061): the editor executable. It opens a native window, creates the graphics device for the chosen backend (DX12 on
# Windows, Vulkan on Linux by default), and runs the editor shell from engine_editor. The standalone runtime does not link this target.
add_executable(game_editor src/main.cpp)
target_link_libraries(game_editor PRIVATE engine_editor engine_game_render engine_gameplay engine_platform engine_rhi engine_core engine_imgui)
target_compile_definitions(game_editor PRIVATE
  ENGINE_EDITOR_FONT_PATH="${PROJECT_SOURCE_DIR}/third_party/roboto/src/hinted/Roboto-Regular.ttf"
  ENGINE_EDITOR_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders")
if(ENGINE_BUILD_SHADER_TOOLCHAIN)
  add_dependencies(game_editor engine_shader_editor_imgui_vertex engine_shader_editor_imgui_pixel)
endif()
# The Build Game command copies the standalone runtime into the package, so the runtime is built first.
add_dependencies(game_editor game_runtime)
engine_configure_first_party(game_editor)
