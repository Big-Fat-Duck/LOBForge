if(NOT DEFINED PROGRAM OR NOT DEFINED EXPECTED OR NOT DEFINED CASE)
  message(FATAL_ERROR "PROGRAM, EXPECTED, and CASE are required")
endif()

if(CASE STREQUAL "usage")
  execute_process(COMMAND "${PROGRAM}" RESULT_VARIABLE actual OUTPUT_QUIET ERROR_QUIET)
elseif(CASE STREQUAL "io")
  execute_process(COMMAND "${PROGRAM}" --input "${INPUT}" --strict RESULT_VARIABLE actual
                  OUTPUT_QUIET ERROR_QUIET)
elseif(CASE STREQUAL "strict")
  execute_process(COMMAND "${PROGRAM}" --input "${INPUT}" --strict RESULT_VARIABLE actual
                  OUTPUT_QUIET ERROR_QUIET)
elseif(CASE STREQUAL "research_usage")
  execute_process(
    COMMAND "${PROGRAM}" --input "${INPUT}" --strict --research-format book-event-v1 --output -
    RESULT_VARIABLE actual OUTPUT_QUIET ERROR_QUIET)
else()
  message(FATAL_ERROR "unknown CASE=${CASE}")
endif()

if(NOT actual EQUAL EXPECTED)
  message(FATAL_ERROR "${CASE}: expected exit ${EXPECTED}, got ${actual}")
endif()
