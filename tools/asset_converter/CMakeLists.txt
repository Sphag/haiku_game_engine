# Asset converter (US-019): the converter library and the asset_converter command-line tool. The runtime never links either
# (FR-03): importers and cooking stay in the tool, and the runtime reads only cooked files.
#
# The vendored nlohmann/json is a system include private to the converter sources, so its warnings stay out of the
# first-party -Werror policy.
#
# Assimp (US-020) is built from third_party/assimp only here, as a static library that only the converter links. Just the
# glTF, FBX, and OBJ importers are compiled; exporters, tools, samples, tests, and docs are off. Its zlib comes from the
# vendored contrib sources, so the build needs no system zlib. These are normal variables, so they apply to this
# directory's Assimp build only and do not leak into the rest of the project.
set(BUILD_SHARED_LIBS OFF)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF)
set(ASSIMP_BUILD_GLTF_IMPORTER ON)
set(ASSIMP_BUILD_FBX_IMPORTER ON)
set(ASSIMP_BUILD_OBJ_IMPORTER ON)
set(ASSIMP_NO_EXPORT ON)
set(ASSIMP_BUILD_ZLIB ON)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF)
set(ASSIMP_BUILD_TESTS OFF)
set(ASSIMP_BUILD_SAMPLES OFF)
set(ASSIMP_BUILD_DOCS OFF)
set(ASSIMP_INSTALL OFF)
set(ASSIMP_WARNINGS_AS_ERRORS OFF)
add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/assimp assimp)

# The BC7 encoder and decoder (US-021) come from bc7enc_rdo. They build as their own library, outside the first-party
# warning policy. The converter's BC4/BC5 codec is first-party code.
add_library(engine_bc7_vendor STATIC
  ${PROJECT_SOURCE_DIR}/third_party/bc7enc_rdo/bc7enc.cpp
)
target_include_directories(engine_bc7_vendor SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/bc7enc_rdo)

# meshoptimizer (US-023) builds from its vendored library sources for LOD simplification, vertex ordering, and meshlets. It
# is outside the first-party warning policy, and only the converter's cooking code uses it.
add_library(engine_meshopt_vendor STATIC
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/allocator.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/clusterizer.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/indexanalyzer.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/indexcodec.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/indexgenerator.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/meshletcodec.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/meshletutils.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/opacitymap.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/overdrawoptimizer.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/partition.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/quantization.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/rasterizer.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/remesher.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/simplifier.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/spatialorder.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/stripifier.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/tangentspace.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/vcacheoptimizer.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/vertexcodec.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/vertexfilter.cpp
  ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src/vfetchoptimizer.cpp
)
target_include_directories(engine_meshopt_vendor SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/meshoptimizer/src)

# Source watching (US-025) uses ReadDirectoryChangesW on Windows and inotify on Linux. Other platforms are not targets.
if(WIN32)
  set(ENGINE_WATCH_BACKEND src/watch_win32.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(ENGINE_WATCH_BACKEND src/watch_linux.cpp)
else()
  message(FATAL_ERROR "asset_converter: source watching is implemented for Windows and Linux only")
endif()

add_library(engine_converter STATIC
  src/watch.cpp
  ${ENGINE_WATCH_BACKEND}
  src/importer.cpp
  src/converter.cpp
  src/model_importer.cpp
  src/mesh_cooking.cpp
  src/texture_importer.cpp
  src/bc7_encode.cpp
  src/gi_importer.cpp
  src/world_partition.cpp
)
target_include_directories(engine_converter PUBLIC include)
target_include_directories(engine_converter SYSTEM PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/single_include
  ${PROJECT_SOURCE_DIR}/third_party/stb
  ${PROJECT_SOURCE_DIR}/third_party/bc7enc_rdo
)
target_link_libraries(engine_converter PUBLIC engine_core engine_assets engine_gi engine_world engine_model_format engine_texture_codec)
target_link_libraries(engine_converter PRIVATE assimp engine_bc7_vendor engine_meshopt_vendor)
engine_configure_first_party(engine_converter)

add_executable(asset_converter src/main.cpp)
target_link_libraries(asset_converter PRIVATE engine_converter)
engine_configure_first_party(asset_converter)
