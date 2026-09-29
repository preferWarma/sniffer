if(NOT DEFINED READER_MEMORY_EXE OR NOT DEFINED READER_MEMORY_TEST_DIR)
  message(FATAL_ERROR "reader-memory smoke test requires executable and test directory")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef memory_suffix)
set(segment_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.seg")
set(parquet_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.parquet")
set(parquet_zstd_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.zstd.parquet")
set(wide_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.wide.seg")

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
  COMMAND "${READER_MEMORY_EXE}" "--generate-parquet=${parquet_path}" --rows=1024
  RESULT_VARIABLE parquet_generate_result
  OUTPUT_VARIABLE parquet_generate_output
  ERROR_VARIABLE parquet_generate_error
)
if(NOT parquet_generate_result EQUAL 0)
  file(REMOVE "${segment_path}" "${parquet_path}")
  message(FATAL_ERROR "Parquet generation failed: ${parquet_generate_output}${parquet_generate_error}")
endif()

execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--generate-parquet=${parquet_path}" --rows=1024
  RESULT_VARIABLE parquet_overwrite_result
  OUTPUT_QUIET
  ERROR_QUIET
)
if(parquet_overwrite_result EQUAL 0)
  file(REMOVE "${segment_path}" "${parquet_path}")
  message(FATAL_ERROR "Parquet generation unexpectedly overwrote an existing file")
endif()

execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--generate-parquet-zstd=${parquet_zstd_path}" --rows=1024
  RESULT_VARIABLE parquet_zstd_generate_result
  OUTPUT_VARIABLE parquet_zstd_generate_output
  ERROR_VARIABLE parquet_zstd_generate_error
)
if(NOT parquet_zstd_generate_result EQUAL 0)
  file(REMOVE "${segment_path}" "${parquet_path}" "${parquet_zstd_path}")
  message(FATAL_ERROR "Parquet ZSTD generation failed: ${parquet_zstd_generate_output}${parquet_zstd_generate_error}")
endif()

execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--generate=${wide_path}" --rows=16384
          --unprojected-binary-bytes=256
  RESULT_VARIABLE wide_generate_result
  OUTPUT_VARIABLE wide_generate_output
  ERROR_VARIABLE wide_generate_error
)
if(NOT wide_generate_result EQUAL 0)
  file(REMOVE "${segment_path}" "${parquet_path}" "${parquet_zstd_path}" "${wide_path}")
  message(FATAL_ERROR "wide Segment generation failed: ${wide_generate_output}${wide_generate_error}")
endif()

execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--segment=${wide_path}" --rows=16384
          --buffer-budget-bytes=1048576
          "--benchmark_filter=^BoundedParallelReaderScan/4/real_time$" --benchmark_dry_run
  RESULT_VARIABLE wide_scan_result
  OUTPUT_VARIABLE wide_scan_output
  ERROR_VARIABLE wide_scan_error
)
if(NOT wide_scan_result EQUAL 0 OR
   NOT "${wide_scan_output}${wide_scan_error}" MATCHES "parallel_workers_started=2" OR
   "${wide_scan_output}${wide_scan_error}" MATCHES "ERROR OCCURRED")
  file(REMOVE "${segment_path}" "${parquet_path}" "${parquet_zstd_path}" "${wide_path}")
  message(FATAL_ERROR "wide budget benchmark did not start workers: ${wide_scan_output}${wide_scan_error}")
endif()

execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--segment=${segment_path}" "--parquet=${parquet_path}"
          "--parquet-zstd=${parquet_zstd_path}" --rows=1024
          --benchmark_dry_run
  RESULT_VARIABLE scan_result
  OUTPUT_VARIABLE scan_output
  ERROR_VARIABLE scan_error
)
file(REMOVE "${segment_path}" "${parquet_path}" "${parquet_zstd_path}" "${wide_path}")
if(NOT scan_result EQUAL 0)
  message(FATAL_ERROR "scan failed: ${scan_output}${scan_error}")
endif()
if(NOT "${scan_output}${scan_error}" MATCHES "ParquetReaderOnlyScan/Uncompressed" OR
   NOT "${scan_output}${scan_error}" MATCHES "ParquetReaderOnlyScan/ZSTD" OR
   "${scan_output}${scan_error}" MATCHES "ERROR OCCURRED")
  message(FATAL_ERROR "Parquet Reader-only benchmark did not complete: ${scan_output}${scan_error}")
endif()
