# The cooked model payload (OWMD): its types, writer, reader, and validator. The converter writes it, and the runtime reads it, so it lives
# outside the converter. The header keeps its asset_converter/ path and engine::converter namespace, so the importer and the tests are
# unchanged. It depends on the core only: the runtime links it without reaching Assimp or the cookers.
add_library(engine_model_format STATIC
  src/model_format.cpp
)
target_include_directories(engine_model_format PUBLIC include)
target_link_libraries(engine_model_format PUBLIC engine_core)
engine_configure_first_party(engine_model_format)

# The 4x4 block codecs for cooked textures: BC4/BC5 encode and decode (first-party) and the BC7 decoder over the vendored bc7decomp. The
# converter encodes with them and the runtime decodes with them, so they live outside the converter. The BC7 encoder stays in the
# converter (engine_bc7_vendor), because the runtime never encodes.
add_library(engine_bc7_decode_vendor STATIC ${PROJECT_SOURCE_DIR}/third_party/bc7enc_rdo/bc7decomp.cpp)
target_include_directories(engine_bc7_decode_vendor SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/bc7enc_rdo)
add_library(engine_texture_codec STATIC
  src/bc5_codec.cpp
  src/bc7_decode.cpp
)
target_include_directories(engine_texture_codec PUBLIC include)
target_link_libraries(engine_texture_codec PUBLIC engine_core PRIVATE engine_bc7_decode_vendor)
engine_configure_first_party(engine_texture_codec)
