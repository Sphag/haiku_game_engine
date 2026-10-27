# The cooked model payload (OWMD): its types, writer, reader, and validator. The converter writes it, and the runtime reads it, so it lives
# outside the converter. The header keeps its asset_converter/ path and engine::converter namespace, so the importer and the tests are
# unchanged. It depends on the core only: the runtime links it without reaching Assimp or the cookers.
add_library(engine_model_format STATIC
  src/model_format.cpp
)
target_include_directories(engine_model_format PUBLIC include)
target_link_libraries(engine_model_format PUBLIC engine_core)
engine_configure_first_party(engine_model_format)
