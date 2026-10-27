# Stages the built DXC host tools for the engine build.
#
#   cmake -DDXC_BINARY_DIR=<dxc build dir> -DDXC_SOURCE_DIR=<third_party/dxc>
#         -DDXC_INSTALL_DIR=<dir> -P cmake/InstallShaderToolchain.cmake
#
# Stages the compiler, its runtime library, and the DXIL validator (dxil.dll / libdxil.so),
# plus the license notices. Layout follows the RUNPATH of dxc: Linux puts libraries in
# lib/ (dxc uses $ORIGIN/../lib); Windows puts DLLs next to dxc.exe in bin/. A missing
# file is a hard error, so an incomplete toolchain is never staged.

if(NOT DXC_BINARY_DIR OR NOT DXC_SOURCE_DIR OR NOT DXC_INSTALL_DIR)
  message(FATAL_ERROR "DXC_BINARY_DIR, DXC_SOURCE_DIR, and DXC_INSTALL_DIR are required")
endif()

if(WIN32)
  set(runtime_files bin/dxc.exe bin/dxcompiler.dll bin/dxil.dll)
else()
  set(runtime_files bin/dxc lib/libdxcompiler.so lib/libdxil.so)
endif()

foreach(file IN LISTS runtime_files)
  set(source "${DXC_BINARY_DIR}/${file}")
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "The DXC build did not produce ${source}")
  endif()
  get_filename_component(destination_dir "${DXC_INSTALL_DIR}/${file}" DIRECTORY)
  file(MAKE_DIRECTORY "${destination_dir}")
  file(COPY_FILE "${source}" "${DXC_INSTALL_DIR}/${file}")
endforeach()

foreach(notice LICENSE.TXT ThirdPartyNotices.txt)
  set(source "${DXC_SOURCE_DIR}/${notice}")
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "Missing DXC license notice: ${source}")
  endif()
  file(MAKE_DIRECTORY "${DXC_INSTALL_DIR}/licenses")
  file(COPY_FILE "${source}" "${DXC_INSTALL_DIR}/licenses/DirectXShaderCompiler-${notice}")
endforeach()
