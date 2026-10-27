# Host shader toolchain (US-003).
#
# DirectXShaderCompiler is built from third_party/dxc as a separate CMake project
# (ExternalProject). Its compiler, flags, and language level are its own: the
# engine compile policy in EngineTargets.cmake (C++20, -Werror, /WX) never reaches
# vendored compiler code.
#
#   engine_check_shader_toolchain_prereqs()   configure-time check of build tools and OS SDK headers
#   engine_add_shader_host_toolchain()        builds dxc, dxcompiler, and the DXIL validator
#   engine_add_hlsl_shader(<name> ...)        compiles one HLSL source to DXIL and SPIR-V
#
# Shaders are always compiled with ENGINE_DXC_EXECUTABLE in the build tree. dxc is never
# looked up on PATH. The build runs offline: every input comes from third_party/.

option(ENGINE_BUILD_SHADER_TOOLCHAIN "Build the pinned DXC host toolchain and compile HLSL shaders" ON)

set(ENGINE_DXC_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/dxc")
set(ENGINE_DXC_BUILD_DIR "${CMAKE_BINARY_DIR}/host-tools/dxc")
set(ENGINE_SHADER_TOOLCHAIN_DIR "${CMAKE_BINARY_DIR}/tools/shader-toolchain")
set(ENGINE_DXC_EXECUTABLE "${ENGINE_SHADER_TOOLCHAIN_DIR}/bin/dxc${CMAKE_EXECUTABLE_SUFFIX}")

# Files that make up the staged tool. The validator (dxil) must sit beside the compiler
# library: without it dxc cannot sign DXIL.
if(WIN32)
  set(ENGINE_DXC_RUNTIME_FILES "bin/dxc.exe" "bin/dxcompiler.dll" "bin/dxil.dll")
else()
  set(ENGINE_DXC_RUNTIME_FILES "bin/dxc" "lib/libdxcompiler.so" "lib/libdxil.so")
endif()

# Diagnoses missing tools and OS SDK pieces before the large DXC build starts.
function(engine_check_shader_toolchain_prereqs)
  find_program(ENGINE_NINJA_EXECUTABLE NAMES ninja ninja-build)
  if(NOT ENGINE_NINJA_EXECUTABLE)
    message(FATAL_ERROR
      "The shader toolchain is built with Ninja, but no ninja executable was found.\n"
      "Install Ninja and put it on PATH, or set ENGINE_NINJA_EXECUTABLE.")
  endif()

  # SPIRV-Tools generates its registry and language tables with Python at build time.
  find_package(Python3 3.8 COMPONENTS Interpreter)
  if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR
      "The shader toolchain needs Python 3.8 or newer for SPIRV-Tools code generation.\n"
      "Install Python 3 and put it on PATH, or pass -DPython3_EXECUTABLE=<path>.")
  endif()

  include(CheckCXXSourceCompiles)
  if(WIN32)
    check_cxx_source_compiles("#include <windows.h>\nint main() { return 0; }" ENGINE_HAVE_WINDOWS_SDK)
    if(NOT ENGINE_HAVE_WINDOWS_SDK)
      message(FATAL_ERROR
        "The shader toolchain needs the Windows SDK (<windows.h> not found).\n"
        "Install the Windows 10/11 SDK with the Visual Studio Installer (Desktop development with C++),\n"
        "then configure from a Visual Studio x64 developer environment.")
    endif()
    # dxcompiler links the ATL static library, which ships with the MSVC build tools, not the SDK.
    check_cxx_source_compiles("#include <windows.h>\n#include <atlbase.h>\nint main() { return 0; }"
      ENGINE_HAVE_ATL)
    if(NOT ENGINE_HAVE_ATL)
      message(FATAL_ERROR
        "The shader toolchain needs ATL (<atlbase.h> not found).\n"
        "Install \"C++ ATL for latest v143 build tools\" with the Visual Studio Installer.")
    endif()
  else()
    set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_DL_LIBS})
    check_cxx_source_compiles("#include <dlfcn.h>\nint main() { return dlopen(nullptr, RTLD_LAZY) ? 0 : 1; }"
      ENGINE_HAVE_DLOPEN)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(NOT ENGINE_HAVE_DLOPEN)
      message(FATAL_ERROR
        "The shader toolchain needs the glibc development headers and libdl (<dlfcn.h>, dlopen).\n"
        "Install libc6-dev and rerun configure.")
    endif()
  endif()
endfunction()

# Builds the pinned DXC tools as a separate CMake project and stages them under
# ENGINE_SHADER_TOOLCHAIN_DIR. The host compilers are passed in explicitly; engine
# flags such as warnings-as-errors and CMAKE_CXX_FLAGS are not forwarded.
function(engine_add_shader_host_toolchain)
  include(ExternalProject)

  set(dxc_host_args
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DLLVM_TARGETS_TO_BUILD=None
    -DLLVM_INCLUDE_TESTS=OFF
    -DLLVM_INCLUDE_EXAMPLES=OFF
    -DLLVM_INCLUDE_DOCS=OFF
    -DLLVM_APPEND_VC_REV=OFF
    -DLLVM_ENABLE_WERROR=OFF
    -DLLVM_ENABLE_EH=ON
    -DLLVM_ENABLE_RTTI=ON
    -DHLSL_INCLUDE_TESTS=OFF
    -DHLSL_BUILD_DXILCONV=OFF
    -DHLSL_DISABLE_SOURCE_GENERATION=ON
    -DCLANG_INCLUDE_TESTS=OFF
    -DCLANG_ENABLE_ARCMT=OFF
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF
    -DLIBCLANG_BUILD_STATIC=OFF
    -DENABLE_SPIRV_CODEGEN=ON
    -DDXC_SPIRV_HEADERS_DIR=${CMAKE_SOURCE_DIR}/third_party/spirv_headers
    -DDXC_SPIRV_TOOLS_DIR=${CMAKE_SOURCE_DIR}/third_party/spirv_tools
    -DCMAKE_MAKE_PROGRAM=${ENGINE_NINJA_EXECUTABLE}
  )
  if(NOT WIN32)
    # The vendored tree has no autoconf/config.guess, so the host triple is given directly.
    # Vendored DirectX-Headers stand in for the Windows SDK's d3d12 headers.
    list(APPEND dxc_host_args
      "-DLLVM_INFERRED_HOST_TRIPLE=${CMAKE_SYSTEM_PROCESSOR}-pc-linux-gnu"
      -DDIRECTX_HEADER_INCLUDE_DIR=${CMAKE_SOURCE_DIR}/third_party/directx_headers/include)
  endif()

  set(dxc_host_outputs "")
  foreach(file IN LISTS ENGINE_DXC_RUNTIME_FILES)
    list(APPEND dxc_host_outputs "${ENGINE_DXC_BUILD_DIR}/${file}")
  endforeach()

  # The generators (llvm-build, hctdb, SPIRV-Tools tables) are Python scripts that would write
  # __pycache__ into third_party/, which the vendor validator rejects, so bytecode is disabled.
  set(no_pycache ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1)

  ExternalProject_Add(engine_dxc_host
    SOURCE_DIR "${ENGINE_DXC_SOURCE_DIR}"
    BINARY_DIR "${ENGINE_DXC_BUILD_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    CONFIGURE_COMMAND ${no_pycache} ${CMAKE_COMMAND} -G Ninja ${dxc_host_args} <SOURCE_DIR>
    BUILD_COMMAND ${no_pycache} ${CMAKE_COMMAND} --build <BINARY_DIR> --target dxc dxcompiler dxildll
    INSTALL_COMMAND ${CMAKE_COMMAND}
      "-DDXC_BINARY_DIR=<BINARY_DIR>"
      "-DDXC_SOURCE_DIR=${ENGINE_DXC_SOURCE_DIR}"
      "-DDXC_INSTALL_DIR=${ENGINE_SHADER_TOOLCHAIN_DIR}"
      -P "${CMAKE_SOURCE_DIR}/cmake/InstallShaderToolchain.cmake"
    # The sub-build is incremental; BUILD_ALWAYS makes source edits under third_party/dxc take effect.
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS ${dxc_host_outputs}
    USES_TERMINAL_BUILD ON
  )
endfunction()

# engine_add_hlsl_shader(<name> SOURCE <file> ENTRY <function> PROFILE <target>)
# engine_add_hlsl_shader(<name> SOURCE <file> PROFILE lib_6_3 DXIL_LIBRARY)
#
# Produces <build>/shaders/<name>.dxil and <build>/shaders/<name>.spv with the pinned dxc,
# as the engine_shader_<name> target (part of ALL). DXIL_LIBRARY (ray tracing libraries) produces the DXIL and a SPIR-V library
# (lib_6_x with -spirv, which carries every ray tracing entry point) instead of one SPIR-V entry.
# Diagnostics are printed by dxc with the source path and line, and they fail the build.
function(engine_add_hlsl_shader name)
  if(NOT ENGINE_BUILD_SHADER_TOOLCHAIN)
    message(FATAL_ERROR "engine_add_hlsl_shader(${name}) requires ENGINE_BUILD_SHADER_TOOLCHAIN=ON")
  endif()

  # DXIL_LIBRARY (US-041, US-042) builds a ray tracing library (lib_6_x) as DXIL for DX12 and as SPIR-V for Vulkan. A library has no
  # entry point on the command line, so both outputs take every exported entry from the source.
  # RAY_QUERY (US-043) enables SPV_KHR_ray_query, which DXC needs before it emits RayQueryKHR for inline ray queries in an entry point.
  cmake_parse_arguments(ARG "DXIL_LIBRARY;RAY_QUERY" "SOURCE;ENTRY;PROFILE" "" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS OR NOT ARG_SOURCE OR NOT ARG_PROFILE OR (NOT ARG_ENTRY AND NOT ARG_DXIL_LIBRARY))
    message(FATAL_ERROR "engine_add_hlsl_shader(${name}) needs SOURCE, PROFILE, and ENTRY (or DXIL_LIBRARY), and no other arguments")
  endif()
  if(ARG_DXIL_LIBRARY AND ARG_ENTRY)
    message(FATAL_ERROR "engine_add_hlsl_shader(${name}): DXIL_LIBRARY takes no ENTRY")
  endif()

  get_filename_component(source "${ARG_SOURCE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  set(out_dir "${CMAKE_BINARY_DIR}/shaders")
  set(dxil "${out_dir}/${name}.dxil")
  set(spirv "${out_dir}/${name}.spv")

  if(ARG_DXIL_LIBRARY)
    set(dxc_entry "")
  else()
    set(dxc_entry -E "${ARG_ENTRY}")
  endif()
  add_custom_command(
    OUTPUT "${dxil}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
    COMMAND "${ENGINE_DXC_EXECUTABLE}" -nologo -T "${ARG_PROFILE}" ${dxc_entry} -Fo "${dxil}" "${source}"
    DEPENDS "${source}" engine_dxc_host
    COMMENT "DXIL ${name}"
    VERBATIM)

  if(ARG_DXIL_LIBRARY)
    add_custom_command(
      OUTPUT "${spirv}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
      COMMAND "${ENGINE_DXC_EXECUTABLE}" -nologo -spirv -fspv-target-env=vulkan1.2 -T "${ARG_PROFILE}" -Fo "${spirv}" "${source}"
      DEPENDS "${source}" engine_dxc_host
      COMMENT "SPIR-V library ${name}"
      VERBATIM)
    add_custom_target(engine_shader_${name} ALL DEPENDS "${dxil}" "${spirv}")
    return()
  endif()

  set(ray_query_extension "")
  if(ARG_RAY_QUERY)
    set(ray_query_extension "-fspv-extension=SPV_KHR_ray_query")
  endif()
  add_custom_command(
    OUTPUT "${spirv}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
    COMMAND "${ENGINE_DXC_EXECUTABLE}" -nologo -spirv -fspv-target-env=vulkan1.2 -fspv-extension=SPV_EXT_mesh_shader ${ray_query_extension}
      -T "${ARG_PROFILE}" -E "${ARG_ENTRY}" -Fo "${spirv}" "${source}"
    DEPENDS "${source}" engine_dxc_host
    COMMENT "SPIR-V ${name}"
    VERBATIM)

  add_custom_target(engine_shader_${name} ALL DEPENDS "${dxil}" "${spirv}")
endfunction()
