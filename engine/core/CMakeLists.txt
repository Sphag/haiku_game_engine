add_library(engine_core STATIC
  src/assert.cpp
  src/clock.cpp
  src/file_io.cpp
  src/guid.cpp
  src/jobs.cpp
  src/log.cpp
  src/path.cpp
  src/status.cpp
  src/utf8.cpp
)
find_package(Threads REQUIRED)
target_include_directories(engine_core PUBLIC include)
# The job system (jobs.hpp) runs tasks on std::thread workers.
target_link_libraries(engine_core PUBLIC Threads::Threads)
# GLM is header-only math (coordinates.hpp). It is included as a system directory so its warnings stay out of -Werror.
target_include_directories(engine_core SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/glm)
engine_configure_first_party(engine_core)
