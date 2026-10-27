# Standalone runtime round trip (US-060). The steps are: cook a fixture project, pack its start scene, then start the packed game
# from an empty working directory. That directory holds no source assets, no build tree, and no editor, so the run proves the
# runtime reads cooked files only. The same run also checks that a start scene which is not a root is refused.
#
# Inputs (all required): CONVERTER, PACKER, RUNTIME, PROJECT, START_SCENE, PALETTE_GUID, SCRATCH.
foreach(var CONVERTER PACKER RUNTIME PROJECT START_SCENE PALETTE_GUID SCRATCH)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${SCRATCH}")
set(COOKED "${SCRATCH}/cooked")
set(PACK_DIR "${SCRATCH}/pack")
set(RUN_DIR "${SCRATCH}/empty_run_dir")
set(PACK "${PACK_DIR}/game.pack")
file(MAKE_DIRECTORY "${COOKED}" "${PACK_DIR}" "${RUN_DIR}")

function(run_checked label expected_exit expected_text)
  execute_process(COMMAND ${ARGN}
    WORKING_DIRECTORY "${RUN_DIR}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
  if(NOT rc EQUAL expected_exit)
    message(FATAL_ERROR "${label}: exit ${rc}, expected ${expected_exit}\n${out}${err}")
  endif()
  if(NOT "${expected_text}" STREQUAL "" AND NOT "${out}${err}" MATCHES "${expected_text}")
    message(FATAL_ERROR "${label}: output does not contain '${expected_text}'\n${out}${err}")
  endif()
endfunction()

run_checked("cook" 0 "\"status\": \"ok\""
  "${CONVERTER}" cook --project "${PROJECT}" --output "${COOKED}" --report -)

run_checked("pack" 0 "packed 2 assets"
  "${PACKER}" --project "${PROJECT}" --cooked "${COOKED}" --root "${START_SCENE}" --output "${PACK}")

run_checked("start from an empty directory" 0 "started scene ${START_SCENE} entities=1 packs=1"
  "${RUNTIME}" --pack "${PACK}" --start-scene "${START_SCENE}" --backend vulkan --profile native)

run_checked("palette is a dependency, not a root" 1 "is not a root of any pack"
  "${RUNTIME}" --pack "${PACK}" --start-scene "${PALETTE_GUID}")

if(NOT EXISTS "${PACK}")
  message(FATAL_ERROR "pack was not written")
endif()
