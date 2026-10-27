# Built-in components (engine.mesh_renderer, engine.light, ...) and GameWorld, the glue that turns them into running systems (physics
# bodies, Lua scripts, audio). The editor's Play session and the standalone runtime share it. It never links the editor or the converter, so
# engine/runtime/CMakeLists.txt's editor-free check still holds.
add_library(engine_gameplay STATIC
  src/animation_binding.cpp
  src/components.cpp
  src/game_world.cpp
)
target_include_directories(engine_gameplay PUBLIC include)
target_link_libraries(engine_gameplay PUBLIC engine_core engine_scene engine_ecs engine_physics engine_script engine_audio engine_animation engine_model_format)
engine_configure_first_party(engine_gameplay)
