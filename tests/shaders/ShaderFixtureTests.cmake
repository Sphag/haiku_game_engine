# Checks the host shader toolchain against the HLSL fixtures.
#
#   MODE=outputs     DXIL and SPIRV name the build outputs of the fixture. Checks the
#                    container and SPIR-V magic and that the DXIL carries a validator digest.
#   MODE=diagnostic  DXC, SOURCE, and EXPECT_LINE. dxc compiles the fallible fixture; the
#                    run must fail and report "<SOURCE>:<EXPECT_LINE>:" with "error:".
#
# Inputs (-D): MODE, DXC, DXIL, SPIRV, SOURCE, EXPECT_LINE, SCRATCH_DIR

if(NOT MODE)
  message(FATAL_ERROR "MODE must be outputs or diagnostic")
endif()

if(MODE STREQUAL "outputs")
  foreach(output IN ITEMS "${DXIL}" "${SPIRV}")
    if(NOT EXISTS "${output}")
      message(FATAL_ERROR "missing shader output: ${output}")
    endif()
    file(SIZE "${output}" output_size)
    if(output_size EQUAL 0)
      message(FATAL_ERROR "empty shader output: ${output}")
    endif()
  endforeach()

  # A DXIL container starts with the fourcc "DXBC".
  file(READ "${DXIL}" dxil_head HEX LIMIT 20)
  if(NOT dxil_head MATCHES "^44584243")
    message(FATAL_ERROR "${DXIL} is not a DXBC container (header ${dxil_head})")
  endif()
  # The 16-byte container digest follows the fourcc. dxc writes it only when the DXIL
  # validator (dxil.dll / libdxil.so) has run; unvalidated output keeps it all zero.
  string(SUBSTRING "${dxil_head}" 8 32 dxil_digest)
  if(dxil_digest STREQUAL "00000000000000000000000000000000")
    message(FATAL_ERROR "${DXIL} is not validated: the DXIL validator digest is zero")
  endif()

  # SPIR-V magic word 0x07230203, stored little-endian.
  file(READ "${SPIRV}" spirv_head HEX LIMIT 4)
  if(NOT spirv_head STREQUAL "03022307")
    message(FATAL_ERROR "${SPIRV} is not SPIR-V (magic ${spirv_head})")
  endif()

  message(STATUS "shader outputs verified: DXIL validated, SPIR-V magic present")

elseif(MODE STREQUAL "diagnostic")
  if(NOT EXISTS "${SOURCE}")
    message(FATAL_ERROR "missing diagnostic fixture: ${SOURCE}")
  endif()

  file(MAKE_DIRECTORY "${SCRATCH_DIR}")
  execute_process(
    COMMAND "${DXC}" -nologo -T cs_6_0 -E main -Fo "${SCRATCH_DIR}/diagnostic.dxil" "${SOURCE}"
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE std_out
    ERROR_VARIABLE std_err
    TIMEOUT 300)

  set(combined "${std_out}${std_err}")
  if(exit_code EQUAL 0)
    message(FATAL_ERROR "dxc accepted ${SOURCE}; the diagnostic fixture must fail")
  endif()

  set(expected "${SOURCE}:${EXPECT_LINE}:")
  string(FIND "${combined}" "${expected}" location)
  if(location EQUAL -1)
    message(FATAL_ERROR
      "expected a diagnostic at '${expected}'\nstdout:\n${std_out}\nstderr:\n${std_err}")
  endif()
  string(FIND "${combined}" "error:" error_position)
  if(error_position EQUAL -1)
    message(FATAL_ERROR "expected an 'error:' diagnostic\nstdout:\n${std_out}\nstderr:\n${std_err}")
  endif()

  message(STATUS "diagnostic reported at ${expected}")

else()
  message(FATAL_ERROR "unknown MODE '${MODE}'")
endif()
