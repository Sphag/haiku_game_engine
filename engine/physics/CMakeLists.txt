# Physics adapter (US-052): one Jolt world per runtime scene, a fixed 60 Hz step, interpolated render transforms, and a queue for
# contact and activation callbacks. Jolt is built from the vendored sources by its own Jolt.cmake, which creates the Jolt target. That
# target keeps the vendor policy (no -Werror); engine_physics is first-party code. The adapter's contract is in
# engine/physics/include/engine/physics/physics_world.hpp.
set(PHYSICS_REPO_ROOT ${PROJECT_SOURCE_DIR}/third_party/jolt)

# Jolt's JPH_USE_* macros must match in Jolt and in every target that includes its headers. Jolt.cmake applies these flags to Jolt's
# public interface, so engine_physics and its consumers compile with the same settings.
set(USE_SSE4_1 ON)
set(USE_SSE4_2 ON)
set(USE_ASSERTS ON)
include(${PHYSICS_REPO_ROOT}/Jolt/Jolt.cmake)
# Jolt.cmake selects C++17. Its math uses __builtin_bit_cast in constexpr functions, which clang 10 (the local Linux compiler) accepts only
# in C++20 mode. The language level changes no Jolt interface, so the target builds at C++20 like the first-party code.
set_target_properties(Jolt PROPERTIES CXX_STANDARD 20 CXX_EXTENSIONS OFF)
# clang 10 also crashes in its debug-type code generator on Jolt's ConvexShape.cpp at -g. Jolt keeps line tables only in Debug;
# first-party code keeps full debug information.
target_compile_options(Jolt PRIVATE $<$<AND:$<CXX_COMPILER_ID:Clang>,$<CONFIG:Debug>>:-g1>)
# Jolt.cmake's precompiled header makes clang 10 reject Jolt's constexpr BitCast (HalfFloat.h) even though the same source compiles
# without it. Clang 11 and later are not affected, so only the older compiler gives up the precompiled header.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
  set_target_properties(Jolt PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
endif()

add_library(engine_physics STATIC
  src/physics_world.cpp
  src/collision_cells.cpp
  src/character.cpp
)
target_include_directories(engine_physics PUBLIC include)
target_link_libraries(engine_physics PUBLIC engine_core engine_ecs engine_world Jolt)
engine_configure_first_party(engine_physics)
