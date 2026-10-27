# In-house entity handles and worlds (US-012). Component pools and queries (US-013) are header-only templates.
# Deferred command buffers and the system scheduler (US-014) run on engine_core's job system.
add_library(engine_ecs STATIC
  src/world.cpp
  src/command_buffer.cpp
  src/systems.cpp
)
target_include_directories(engine_ecs PUBLIC include)
target_link_libraries(engine_ecs PUBLIC engine_core)
engine_configure_first_party(engine_ecs)
