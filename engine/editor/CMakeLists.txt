# Editor shell (US-061): the project, console, workspace, and document model (EditorModel), which needs no graphics or ImGui; the in-house
# ImGui platform/input adapter; the ImGui renderer that draws through engine_rhi for both backends; and the editor UI (docking, menu, panels).
# The asset browser (US-066) links the converter: the editor cooks and previews assets through it. docs/editor.md describes the contract.
# The runtime must not link this library (engine/runtime/CMakeLists.txt checks it).
#
# Dear ImGui (docking branch) is built as engine_imgui from the vendored sources, outside the first-party warning policy, the way lua_vm is.
# Only its core is compiled: the vendored DX12 and Vulkan backends are not used, because the RHI draws the UI for both backends.
add_library(engine_imgui STATIC
  ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui.cpp
  ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
  ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
  ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
)
target_include_directories(engine_imgui SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/imgui)

add_library(engine_editor STATIC
  src/project.cpp
  src/console.cpp
  src/workspace.cpp
  src/editor_model.cpp
  src/asset_browser.cpp
  src/scene_editor.cpp
  src/play_session.cpp
  src/viewport.cpp
  src/imgui_input.cpp
  src/imgui_renderer.cpp
  src/editor_ui.cpp
  src/material_graph_panel.cpp
  src/animation_graph_panel.cpp
  src/terrain_editor.cpp
  src/terrain_panel.cpp
  src/world_editor.cpp
  src/world_panel.cpp
  src/distribution.cpp
)
target_include_directories(engine_editor PUBLIC include)
# nlohmann/json is a system include, so its warnings stay out of the first-party -Werror policy (as in engine_save).
target_include_directories(engine_editor SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/single_include)
target_link_libraries(engine_editor PUBLIC engine_core engine_platform engine_rhi engine_assets engine_scene engine_physics engine_script engine_audio engine_imgui engine_converter engine_material_graph engine_animation engine_world engine_pack engine_gameplay)

# Build Game (US-073) copies the runtime, the shaders, and the notices into the package. The runtime's path is a generator expression, so
# game_editor and engine_tests must depend on game_runtime to have it built first (tools/game_editor and tests/CMakeLists.txt do).
target_compile_definitions(engine_editor PRIVATE
  ENGINE_GAME_RUNTIME_EXE="$<TARGET_FILE:game_runtime>"
  ENGINE_BUILD_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
  ENGINE_NOTICES_FILE="${CMAKE_BINARY_DIR}/distribution/NOTICES.txt")

# The package's NOTICES.txt is generated at configure time from third_party/manifest.json: each pinned dependency's name, version,
# license, and source, followed by its license text when the manifest names a license file. The manifest is a configure dependency.
set(ENGINE_NOTICES_MANIFEST "${PROJECT_SOURCE_DIR}/third_party/manifest.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ENGINE_NOTICES_MANIFEST}")
file(READ "${ENGINE_NOTICES_MANIFEST}" _notices_manifest)
string(JSON _notices_count LENGTH "${_notices_manifest}" dependencies)
set(_notices "Third-party notices for this game package.\nThe engine's pinned dependencies, with the license text each one ships.\n")
math(EXPR _notices_last "${_notices_count} - 1")
foreach(_notices_index RANGE ${_notices_last})
  string(JSON _name GET "${_notices_manifest}" dependencies ${_notices_index} name)
  string(JSON _version GET "${_notices_manifest}" dependencies ${_notices_index} version)
  string(JSON _license GET "${_notices_manifest}" dependencies ${_notices_index} license)
  string(JSON _url GET "${_notices_manifest}" dependencies ${_notices_index} url)
  string(APPEND _notices "\n== ${_name} ${_version} (${_license}) ==\n${_url}\n")
  string(JSON _license_file ERROR_VARIABLE _license_error GET "${_notices_manifest}" dependencies ${_notices_index} licenseFile)
  if(_license_file AND EXISTS "${PROJECT_SOURCE_DIR}/third_party/${_license_file}")
    file(READ "${PROJECT_SOURCE_DIR}/third_party/${_license_file}" _license_text)
    string(APPEND _notices "\n${_license_text}\n")
  endif()
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/distribution/NOTICES.txt" "${_notices}")

engine_configure_first_party(engine_editor)

# The editor's ImGui shaders (imgui.hlsl) are compiled for both backends. The host and the editor tests load them from the build's shader folder.
if(ENGINE_BUILD_SHADER_TOOLCHAIN)
  engine_add_hlsl_shader(editor_imgui_vertex SOURCE shaders/imgui.hlsl ENTRY VSMain PROFILE vs_6_0)
  engine_add_hlsl_shader(editor_imgui_pixel SOURCE shaders/imgui.hlsl ENTRY PSMain PROFILE ps_6_0)
endif()
