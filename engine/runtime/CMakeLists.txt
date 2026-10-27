# Runtime lifecycle and option parsing, and the standalone game runtime (US-060). The engine_runtime library holds the lifecycle and the
# option parser; it depends on the core only. The game_runtime executable loads resource packs and starts a scene. It links the
# pack, asset, and scene libraries, and never the converter or the importers (FR-03).
add_library(engine_runtime STATIC
  src/lifecycle.cpp
  src/runtime_options.cpp
)
target_include_directories(engine_runtime PUBLIC include)
target_link_libraries(engine_runtime PUBLIC engine_core)
engine_configure_first_party(engine_runtime)

# The windowed game: GameRenderer (the scene through SceneRenderer, presented by a buffer copy) and the window loop. Kept apart from
# engine_runtime so the option parser and lifecycle tests do not link the renderer.
add_library(engine_game_render STATIC
  src/game_renderer.cpp
  src/windowed_game.cpp
)
target_include_directories(engine_game_render PUBLIC include)
target_link_libraries(engine_game_render PUBLIC engine_runtime engine_render engine_rhi engine_platform engine_gameplay engine_scene engine_animation engine_world engine_model_format engine_core)
engine_configure_first_party(engine_game_render)

add_executable(game_runtime src/game_main.cpp)
target_link_libraries(game_runtime PRIVATE engine_runtime engine_game_render engine_pack engine_assets engine_scene engine_gameplay engine_core)
engine_configure_first_party(game_runtime)

# FR-03 check at configure time: walk the transitive link graph of the runtime and stop the configure step if it reaches the
# converter, its vendored codecs, or Assimp. A new dependency that pulls one in fails here, not at the first packaged build.
function(engine_assert_runtime_is_editor_free target)
  set(pending ${target})
  set(seen "")
  while(pending)
    list(POP_FRONT pending current)
    if(current IN_LIST seen)
      continue()
    endif()
    list(APPEND seen ${current})
    if(current MATCHES "^(engine_converter|engine_bc7_vendor|engine_meshopt_vendor|assimp|asset_converter)$")
      message(FATAL_ERROR "${target} must not link the asset converter or the importers, but it reaches ${current}")
    endif()
    if(TARGET ${current})
      get_target_property(deps ${current} LINK_LIBRARIES)
      if(deps)
        list(APPEND pending ${deps})
      endif()
    endif()
  endwhile()
endfunction()
engine_assert_runtime_is_editor_free(game_runtime)
