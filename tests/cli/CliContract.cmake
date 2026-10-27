# Runs engine_tests with CLI_ARGS and checks the exit code and expected text.
#
# Inputs (-D): EXECUTABLE (the program under test), CLI_ARGS ('|'-separated), EXPECT_EXIT, EXPECT_TEXT

if(NOT EXECUTABLE OR NOT EXISTS "${EXECUTABLE}")
  message(FATAL_ERROR "EXECUTABLE must name an existing executable: '${EXECUTABLE}'")
endif()

string(REPLACE "|" ";" cli_arg_list "${CLI_ARGS}")

execute_process(
  COMMAND "${EXECUTABLE}" ${cli_arg_list}
  RESULT_VARIABLE exit_code
  OUTPUT_VARIABLE std_out
  ERROR_VARIABLE std_err
  TIMEOUT 300)

set(combined "${std_out}${std_err}")

if(NOT "${exit_code}" STREQUAL "${EXPECT_EXIT}")
  message(FATAL_ERROR
    "${EXECUTABLE} ${CLI_ARGS}: expected exit code ${EXPECT_EXIT}, got '${exit_code}'\n"
    "stdout:\n${std_out}\nstderr:\n${std_err}")
endif()

string(FIND "${combined}" "${EXPECT_TEXT}" text_position)
if(text_position EQUAL -1)
  message(FATAL_ERROR
    "${EXECUTABLE} ${CLI_ARGS}: expected output to contain '${EXPECT_TEXT}'\n"
    "stdout:\n${std_out}\nstderr:\n${std_err}")
endif()

message(STATUS "${EXECUTABLE} ${CLI_ARGS}: exit ${exit_code}, found '${EXPECT_TEXT}'")
