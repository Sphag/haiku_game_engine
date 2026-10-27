# In-house GPU contract (US-026) and its backends. The contract and the CPU reference device are first-party code and run on every
# OS. The DX12 backend (US-027) is Windows-only: it calls the Windows SDK's D3D12 and DXGI ABIs directly, with no vendored sources.
# The Vulkan backend (US-028) runs on both OSes. It compiles against the vendored Vulkan-Headers as a system include, so the headers'
# warnings never reach -Werror, and it loads the Vulkan loader at run time (vulkan-1.dll or libvulkan.so.1), so no import library is
# linked.
# FidelityFX FSR 2 (US-046) is vendored under third_party/fsr2 and compiled here, into the RHI, because the DX12 and Vulkan upscalers are
# backends. Its sources are third-party code: they are compiled with warnings off, and their headers are system includes, so neither
# reaches the -Werror policy that applies to first-party code. The vendor tree has no CMake file (ValidateVendor rejects unlisted files).
#
# The precompiled shader tables of FSR 2 include per-pass *_permutations.h files, which the upstream sources do not contain: the shader
# compiler generates them at build time. Until they are vendored (docs/upscaling.md), FSR 2 is not built, and create_upscaler reports
# Unsupported on both backends.
set(ENGINE_FSR2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/fsr2/src/ffx-fsr2-api")
set(ENGINE_FSR2_AVAILABLE ON)
foreach(pass IN ITEMS accumulate_pass autogen_reactive_pass compute_luminance_pyramid_pass depth_clip_pass lock_pass
                      reconstruct_previous_depth_pass rcas_pass tcr_autogen_pass)
  foreach(backend IN ITEMS vk dx12)
    if(NOT EXISTS "${ENGINE_FSR2_ROOT}/${backend}/shaders/ffx_fsr2_${pass}_permutations.h")
      set(ENGINE_FSR2_AVAILABLE OFF)
    endif()
  endforeach()
endforeach()
if(ENGINE_FSR2_AVAILABLE)
  set(ENGINE_FSR2_SOURCES
    ${ENGINE_FSR2_ROOT}/ffx_assert.cpp
    ${ENGINE_FSR2_ROOT}/ffx_fsr2.cpp
    ${ENGINE_FSR2_ROOT}/vk/ffx_fsr2_vk.cpp
    ${ENGINE_FSR2_ROOT}/vk/shaders/ffx_fsr2_shaders_vk.cpp
  )
  set(ENGINE_FSR2_UPSCALER_SOURCES src/vulkan/vulkan_upscaler.cpp)
else()
  message(STATUS "FSR 2 is not built: its shader permutation headers are not vendored (docs/upscaling.md)")
  set(ENGINE_FSR2_SOURCES "")
  set(ENGINE_FSR2_UPSCALER_SOURCES "")
endif()
if(ENGINE_FSR2_AVAILABLE)
  set(ENGINE_FSR2_AVAILABLE_FLAG 1)
else()
  set(ENGINE_FSR2_AVAILABLE_FLAG 0)
endif()

add_library(engine_rhi STATIC
  src/device.cpp
  src/fsr2_common.cpp
  src/fsr2_common.hpp
  src/object_lifetime.hpp
  src/rhi_types.cpp
  src/upscaler.cpp
  src/validation_device.cpp
  src/vulkan/vulkan_api.cpp
  src/vulkan/vulkan_common.cpp
  src/vulkan/vulkan_command_list.cpp
  src/vulkan/vulkan_device.cpp
  src/vulkan/vulkan_swapchain.cpp
  ${ENGINE_FSR2_UPSCALER_SOURCES}
  ${ENGINE_FSR2_SOURCES}
)
target_include_directories(engine_rhi PUBLIC include)
target_include_directories(engine_rhi PRIVATE src)
target_include_directories(engine_rhi SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/vulkan_headers/include")
target_include_directories(engine_rhi SYSTEM PRIVATE "${ENGINE_FSR2_ROOT}")
target_compile_definitions(engine_rhi PRIVATE ENGINE_FSR2_AVAILABLE=${ENGINE_FSR2_AVAILABLE_FLAG} $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:FFX_GCC=1>)
if(ENGINE_FSR2_AVAILABLE)
  set_source_files_properties(${ENGINE_FSR2_SOURCES} PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
endif()
target_link_libraries(engine_rhi PUBLIC engine_core)
if(UNIX)
  # dlopen/dlsym load the Vulkan loader (libvulkan.so.1) at run time.
  target_link_libraries(engine_rhi PRIVATE ${CMAKE_DL_LIBS})
endif()
if(WIN32)
  target_sources(engine_rhi PRIVATE
    src/dx12/dx12_common.cpp
    src/dx12/dx12_command_list.cpp
    src/dx12/dx12_descriptors.cpp
    src/dx12/dx12_device.cpp
    src/dx12/dx12_pipeline.cpp
    src/dx12/dx12_raytracing.cpp
    src/dx12/dx12_swapchain.cpp
  )
  if(ENGINE_FSR2_AVAILABLE)
    target_sources(engine_rhi PRIVATE
      src/dx12/dx12_upscaler.cpp
      ${ENGINE_FSR2_ROOT}/dx12/ffx_fsr2_dx12.cpp
      ${ENGINE_FSR2_ROOT}/dx12/shaders/ffx_fsr2_shaders_dx12.cpp
    )
    set_source_files_properties(${ENGINE_FSR2_ROOT}/dx12/ffx_fsr2_dx12.cpp ${ENGINE_FSR2_ROOT}/dx12/shaders/ffx_fsr2_shaders_dx12.cpp
      PROPERTIES COMPILE_OPTIONS "/W0")
  endif()
  # Windows SDK import libraries for the D3D12 device, DXGI factory, and the IID_* globals (listed as permitted OS ABIs).
  target_link_libraries(engine_rhi PRIVATE d3d12 dxgi dxguid)
endif()
engine_configure_first_party(engine_rhi)
