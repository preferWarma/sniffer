if(NOT DEFINED READER_MEMORY_EXE OR NOT DEFINED READER_MEMORY_TEST_DIR)
  message(FATAL_ERROR "reader-memory smoke test requires executable and test directory")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef memory_suffix)
set(segment_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.seg")

execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--generate=${segment_path}" --rows=1024
  RESULT_VARIABLE generate_result
  OUTPUT_VARIABLE generate_output
  ERROR_VARIABLE generate_error
)
if(NOT generate_result EQUAL 0)
  file(REMOVE "${segment_path}")
  message(FATAL_ERROR "generation failed: ${generate_output}${generate_error}")
endif()

execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--generate=${segment_path}" --rows=1024
  RESULT_VARIABLE overwrite_result
  OUTPUT_QUIET
  ERROR_QUIET
)
if(overwrite_result EQUAL 0)
  file(REMOVE "${segment_path}")
  message(FATAL_ERROR "generation unexpectedly overwrote an existing Segment")
endif()

execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--segment=${segment_path}" --rows=1024
          --benchmark_dry_run
  RESULT_VARIABLE scan_result
  OUTPUT_VARIABLE scan_output
  ERROR_VARIABLE scan_error
)
file(REMOVE "${segment_path}")
if(NOT scan_result EQUAL 0)
  message(FATAL_ERROR "scan failed: ${scan_output}${scan_error}")
endif()
