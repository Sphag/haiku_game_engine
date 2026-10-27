cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not supported. Configure with a preset, for example: cmake --preset linux-dev")
endif()

project(OpenWorldEngine
  VERSION 0.1.0
  DESCRIPTION "C++20 single-player open-world game engine"
  LANGUAGES C CXX)

get_property(engine_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT engine_multi_config)
  message(FATAL_ERROR "This project requires a multi-config generator such as Ninja Multi-Config. Configure with a preset, for example: cmake --preset linux-dev")
endif()

# Configuration is offline: vendored sources are checked, never downloaded.
include(cmake/ValidateVendor.cmake)
engine_validate_vendor("${CMAKE_CURRENT_SOURCE_DIR}/third_party")

include(cmake/EngineTargets.cmake)

# Host shader toolchain: the pinned DXC is built as its own CMake project, outside the
# engine compile policy. Prerequisites are checked first so a missing SDK fails fast.
include(cmake/ShaderToolchain.cmake)
if(ENGINE_BUILD_SHADER_TOOLCHAIN)
  engine_check_shader_toolchain_prereqs()
  engine_add_shader_host_toolchain()
endif()

# Catch2 is vendored as ordinary sources; its own warnings are not promoted to errors.
# Its library is compiled at the same language level as its C++20 consumers so that
# feature-dependent stringification (for example std::string_view) links consistently.
add_subdirectory(third_party/catch2)
set_target_properties(Catch2 PROPERTIES CXX_STANDARD 20 CXX_EXTENSIONS OFF)

add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/ecs)
add_subdirectory(engine/scene)
add_subdirectory(engine/gameplay)
add_subdirectory(engine/assets)
add_subdirectory(engine/rhi)
add_subdirectory(engine/shader)
add_subdirectory(engine/gi)
add_subdirectory(engine/world)
add_subdirectory(engine/physics)
add_subdirectory(engine/render)
add_subdirectory(engine/animation)
add_subdirectory(engine/audio)
add_subdirectory(engine/ui)
add_subdirectory(engine/script)
add_subdirectory(engine/save)
add_subdirectory(engine/pack)
add_subdirectory(engine/material_graph)
add_subdirectory(tools/asset_converter)
add_subdirectory(tools/asset_packer)
add_subdirectory(tools/benchmark)
add_subdirectory(engine/runtime)
add_subdirectory(engine/editor)
add_subdirectory(tools/game_editor)

enable_testing()
add_subdirectory(tests)
