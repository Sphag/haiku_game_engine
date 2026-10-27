# Resource packer (US-060): the asset_packer command-line tool. It packages cooked files into a resource pack. It links the
# pack and assets libraries but not the converter: it never imports or cooks, so a missing cooked file is an error.
add_executable(asset_packer src/main.cpp)
target_link_libraries(asset_packer PRIVATE engine_pack engine_assets engine_core)
engine_configure_first_party(asset_packer)
