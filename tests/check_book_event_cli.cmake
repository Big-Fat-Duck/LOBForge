if(NOT DEFINED PROGRAM OR NOT DEFINED INPUT OR NOT DEFINED EXPECTED)
  message(FATAL_ERROR "PROGRAM, INPUT, and EXPECTED are required")
endif()

execute_process(
  COMMAND "${PROGRAM}" --input "${INPUT}" --session-date 2026-08-24 --strict
          --research-format book-event-v1 --depth 10 --output -
  RESULT_VARIABLE result OUTPUT_VARIABLE actual ERROR_VARIABLE diagnostics)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "book_event/v1 CLI failed (${result}): ${diagnostics}")
endif()
file(READ "${EXPECTED}" expected)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR "book_event/v1 output differs from byte-exact golden")
endif()
