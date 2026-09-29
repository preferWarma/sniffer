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

set(binary_segment_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.binary.seg")
set(binary_parquet_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.binary.parquet")
set(binary_zstd_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.binary.zstd.parquet")
foreach(binary_case IN ITEMS segment parquet zstd)
  if(binary_case STREQUAL "segment")
    set(binary_generate "--generate=${binary_segment_path}")
  elseif(binary_case STREQUAL "parquet")
    set(binary_generate "--generate-parquet=${binary_parquet_path}")
  else()
    set(binary_generate "--generate-parquet-zstd=${binary_zstd_path}")
  endif()
  execute_process(
    COMMAND "${READER_MEMORY_EXE}" "${binary_generate}" --rows=16384
            --projected-binary-bytes=128
    RESULT_VARIABLE binary_generate_result
    OUTPUT_VARIABLE binary_generate_output
    ERROR_VARIABLE binary_generate_error
  )
  if(NOT binary_generate_result EQUAL 0)
    file(REMOVE "${binary_segment_path}" "${binary_parquet_path}" "${binary_zstd_path}")
    message(FATAL_ERROR "binary ${binary_case} generation failed: ${binary_generate_output}${binary_generate_error}")
  endif()
endforeach()

execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--segment=${binary_segment_path}"
          "--parquet=${binary_parquet_path}" "--parquet-zstd=${binary_zstd_path}"
          --rows=16384 --projected-binary-bytes=128
          "--benchmark_filter=^(ReaderOnlyScan|BoundedParallelReaderScan/4|ParquetReaderOnlyScan/Uncompressed|ParquetReaderOnlyScan/ZSTD)/real_time$"
          --benchmark_dry_run
  RESULT_VARIABLE binary_scan_result
  OUTPUT_VARIABLE binary_scan_output
  ERROR_VARIABLE binary_scan_error
)
file(REMOVE "${binary_segment_path}" "${binary_parquet_path}" "${binary_zstd_path}")
if(NOT binary_scan_result EQUAL 0 OR
   NOT "${binary_scan_output}${binary_scan_error}" MATCHES "parallel_workers_started=2" OR
   NOT "${binary_scan_output}${binary_scan_error}" MATCHES "ParquetReaderOnlyScan/ZSTD" OR
   "${binary_scan_output}${binary_scan_error}" MATCHES "ERROR OCCURRED")
  message(FATAL_ERROR "binary projection scan failed: ${binary_scan_output}${binary_scan_error}")
endif()

set(projected_segment_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.projected.seg")
set(projected_parquet_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.projected.parquet")
set(projected_zstd_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.projected.zstd.parquet")
foreach(projected_case IN ITEMS segment parquet zstd)
  if(projected_case STREQUAL "segment")
    set(projected_generate "--generate=${projected_segment_path}")
  elseif(projected_case STREQUAL "parquet")
    set(projected_generate "--generate-parquet=${projected_parquet_path}")
  else()
    set(projected_generate "--generate-parquet-zstd=${projected_zstd_path}")
  endif()
  execute_process(
    COMMAND "${READER_MEMORY_EXE}" "${projected_generate}" --rows=16384
            --projected-columns=15
    RESULT_VARIABLE projected_generate_result
    OUTPUT_VARIABLE projected_generate_output
    ERROR_VARIABLE projected_generate_error
  )
  if(NOT projected_generate_result EQUAL 0)
    file(REMOVE "${projected_segment_path}" "${projected_parquet_path}" "${projected_zstd_path}")
    message(FATAL_ERROR "wide projection ${projected_case} generation failed: ${projected_generate_output}${projected_generate_error}")
  endif()
endforeach()

execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--segment=${projected_segment_path}"
          "--parquet=${projected_parquet_path}" "--parquet-zstd=${projected_zstd_path}"
          --rows=16384 --projected-columns=15
          "--benchmark_filter=^(ReaderOnlyScan|BoundedParallelReaderScan/4|ParquetReaderOnlyScan/Uncompressed|ParquetReaderOnlyScan/ZSTD)/real_time$"
          --benchmark_dry_run
  RESULT_VARIABLE projected_scan_result
  OUTPUT_VARIABLE projected_scan_output
  ERROR_VARIABLE projected_scan_error
)
file(REMOVE "${projected_segment_path}" "${projected_parquet_path}" "${projected_zstd_path}")
if(NOT projected_scan_result EQUAL 0 OR
   NOT "${projected_scan_output}${projected_scan_error}" MATCHES "parallel_workers_started=2" OR
   NOT "${projected_scan_output}${projected_scan_error}" MATCHES "ParquetReaderOnlyScan/ZSTD" OR
   "${projected_scan_output}${projected_scan_error}" MATCHES "ERROR OCCURRED")
  message(FATAL_ERROR "wide projection scan failed: ${projected_scan_output}${projected_scan_error}")
endif()

set(binary_wide_segment_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.binary_wide.seg")
set(binary_wide_parquet_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.binary_wide.parquet")
set(binary_wide_zstd_path "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.binary_wide.zstd.parquet")
foreach(binary_wide_case IN ITEMS segment parquet zstd)
  if(binary_wide_case STREQUAL "segment")
    set(binary_wide_generate "--generate=${binary_wide_segment_path}")
  elseif(binary_wide_case STREQUAL "parquet")
    set(binary_wide_generate "--generate-parquet=${binary_wide_parquet_path}")
  else()
    set(binary_wide_generate "--generate-parquet-zstd=${binary_wide_zstd_path}")
  endif()
  execute_process(
    COMMAND "${READER_MEMORY_EXE}" "${binary_wide_generate}" --rows=16384
            --projected-binary-bytes=32 --projected-columns=4
    RESULT_VARIABLE binary_wide_generate_result
    OUTPUT_VARIABLE binary_wide_generate_output
    ERROR_VARIABLE binary_wide_generate_error
  )
  if(NOT binary_wide_generate_result EQUAL 0)
    file(REMOVE "${binary_wide_segment_path}" "${binary_wide_parquet_path}" "${binary_wide_zstd_path}")
    message(FATAL_ERROR "binary-wide ${binary_wide_case} generation failed: ${binary_wide_generate_output}${binary_wide_generate_error}")
  endif()
endforeach()

execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--segment=${binary_wide_segment_path}"
          "--parquet=${binary_wide_parquet_path}" "--parquet-zstd=${binary_wide_zstd_path}"
          --rows=16384 --projected-binary-bytes=32 --projected-columns=4
          "--benchmark_filter=^(ReaderOnlyScan|BoundedParallelReaderScan/4|ParquetReaderOnlyScan/Uncompressed|ParquetReaderOnlyScan/ZSTD)/real_time$"
          --benchmark_dry_run
  RESULT_VARIABLE binary_wide_scan_result
  OUTPUT_VARIABLE binary_wide_scan_output
  ERROR_VARIABLE binary_wide_scan_error
)
foreach(selectivity IN ITEMS 1 100)
  execute_process(
    COMMAND "${READER_MEMORY_EXE}" "--segment=${binary_wide_segment_path}"
            "--parquet=${binary_wide_parquet_path}" "--parquet-zstd=${binary_wide_zstd_path}"
            --rows=16384 --projected-binary-bytes=32 --projected-columns=4
            "--selectivity-percent=${selectivity}"
            "--benchmark_filter=^(ReaderOnlyScan|BoundedParallelReaderScan/4|ParquetReaderOnlyScan/Uncompressed|ParquetReaderOnlyScan/ZSTD)/real_time$"
            --benchmark_dry_run
    RESULT_VARIABLE selectivity_result
    OUTPUT_VARIABLE selectivity_output
    ERROR_VARIABLE selectivity_error
  )
  if(NOT selectivity_result EQUAL 0 OR
     NOT "${selectivity_output}${selectivity_error}" MATCHES "ParquetReaderOnlyScan/ZSTD" OR
     "${selectivity_output}${selectivity_error}" MATCHES "ERROR OCCURRED")
    file(REMOVE "${binary_wide_segment_path}" "${binary_wide_parquet_path}" "${binary_wide_zstd_path}")
    message(FATAL_ERROR "${selectivity}% selectivity scan failed: ${selectivity_output}${selectivity_error}")
  endif()
endforeach()
file(REMOVE "${binary_wide_segment_path}" "${binary_wide_parquet_path}" "${binary_wide_zstd_path}")
if(NOT binary_wide_scan_result EQUAL 0 OR
   NOT "${binary_wide_scan_output}${binary_wide_scan_error}" MATCHES "parallel_workers_started=2" OR
   NOT "${binary_wide_scan_output}${binary_wide_scan_error}" MATCHES "ParquetReaderOnlyScan/ZSTD" OR
   "${binary_wide_scan_output}${binary_wide_scan_error}" MATCHES "ERROR OCCURRED")
  message(FATAL_ERROR "binary-wide projection scan failed: ${binary_wide_scan_output}${binary_wide_scan_error}")
endif()

set(small_rg_segment "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.rg1024.seg")
set(small_rg_parquet "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.rg1024.parquet")
set(small_rg_zstd "${READER_MEMORY_TEST_DIR}/reader_memory_${memory_suffix}.rg1024.zstd.parquet")
foreach(small_rg_case IN ITEMS segment parquet zstd)
  if(small_rg_case STREQUAL "segment")
    set(small_rg_generate "--generate=${small_rg_segment}")
  elseif(small_rg_case STREQUAL "parquet")
    set(small_rg_generate "--generate-parquet=${small_rg_parquet}")
  else()
    set(small_rg_generate "--generate-parquet-zstd=${small_rg_zstd}")
  endif()
  execute_process(
    COMMAND "${READER_MEMORY_EXE}" "${small_rg_generate}" --rows=2048
            --row-group-rows=1024 --projected-binary-bytes=32 --projected-columns=4
    RESULT_VARIABLE small_rg_generate_result
    OUTPUT_VARIABLE small_rg_generate_output
    ERROR_VARIABLE small_rg_generate_error
  )
  if(NOT small_rg_generate_result EQUAL 0)
    file(REMOVE "${small_rg_segment}" "${small_rg_parquet}" "${small_rg_zstd}")
    message(FATAL_ERROR "RG 1024 ${small_rg_case} generation failed: ${small_rg_generate_output}${small_rg_generate_error}")
  endif()
endforeach()

execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--segment=${small_rg_segment}"
          "--parquet=${small_rg_parquet}" "--parquet-zstd=${small_rg_zstd}"
          --rows=2048 --row-group-rows=1024 --projected-binary-bytes=32
          --projected-columns=4 --selectivity-percent=1
          "--benchmark_filter=^(ReaderOnlyScan|BoundedParallelReaderScan/4|ParquetReaderOnlyScan/Uncompressed|ParquetReaderOnlyScan/ZSTD)/real_time$"
          --benchmark_dry_run
  RESULT_VARIABLE small_rg_result
  OUTPUT_VARIABLE small_rg_output
  ERROR_VARIABLE small_rg_error
)
execute_process(
  COMMAND "${READER_MEMORY_EXE}" "--segment=${small_rg_segment}" --rows=2048
          --row-group-rows=8192 --projected-binary-bytes=32 --projected-columns=4
          "--benchmark_filter=^ReaderOnlyScan/real_time$" --benchmark_dry_run
  OUTPUT_VARIABLE mismatched_rg_output
  ERROR_VARIABLE mismatched_rg_error
)
if(APPLE)
  execute_process(
    COMMAND "${READER_MEMORY_EXE}" "--segment=${small_rg_segment}"
            "--parquet=${small_rg_parquet}" "--parquet-zstd=${small_rg_zstd}"
            --rows=2048 --row-group-rows=1024 --projected-binary-bytes=32
            --projected-columns=4 --cache-bypass
            "--benchmark_filter=^ReaderOnlyScan/real_time$"
            --benchmark_dry_run
    RESULT_VARIABLE cache_probe_result
    OUTPUT_VARIABLE cache_probe_output
    ERROR_VARIABLE cache_probe_error
  )
  if(NOT cache_probe_result EQUAL 0 OR
     ("${cache_probe_output}${cache_probe_error}" MATCHES "ERROR OCCURRED" AND
      NOT "${cache_probe_output}${cache_probe_error}" MATCHES "NotImplemented"))
    file(REMOVE "${small_rg_segment}" "${small_rg_parquet}" "${small_rg_zstd}")
    message(FATAL_ERROR "cache-bypass probe failed: ${cache_probe_output}${cache_probe_error}")
  endif()
  if(NOT "${cache_probe_output}${cache_probe_error}" MATCHES "NotImplemented")
    execute_process(
      COMMAND "${READER_MEMORY_EXE}" "--segment=${small_rg_segment}"
            "--parquet=${small_rg_parquet}" "--parquet-zstd=${small_rg_zstd}"
            --rows=2048 --row-group-rows=1024 --projected-binary-bytes=32
            --projected-columns=4 --cache-bypass
            "--benchmark_filter=^(ReaderOnlyScan|BoundedParallelReaderScan/4|ParquetReaderOnlyScan/Uncompressed|ParquetReaderOnlyScan/ZSTD)/real_time$"
            --benchmark_dry_run
      RESULT_VARIABLE cache_bypass_result
      OUTPUT_VARIABLE cache_bypass_output
      ERROR_VARIABLE cache_bypass_error
    )
    if(NOT cache_bypass_result EQUAL 0 OR
       "${cache_bypass_output}${cache_bypass_error}" MATCHES "ERROR OCCURRED")
      file(REMOVE "${small_rg_segment}" "${small_rg_parquet}" "${small_rg_zstd}")
      message(FATAL_ERROR "cache-bypass Reader-only smoke failed: ${cache_bypass_output}${cache_bypass_error}")
    endif()
  endif()
endif()
file(REMOVE "${small_rg_segment}" "${small_rg_parquet}" "${small_rg_zstd}")
if(NOT small_rg_result EQUAL 0 OR
   NOT "${small_rg_output}${small_rg_error}" MATCHES "parallel_workers_started=2" OR
   NOT "${small_rg_output}${small_rg_error}" MATCHES "ParquetReaderOnlyScan/ZSTD" OR
   "${small_rg_output}${small_rg_error}" MATCHES "ERROR OCCURRED" OR
   NOT "${mismatched_rg_output}${mismatched_rg_error}" MATCHES "ERROR OCCURRED")
  message(FATAL_ERROR "RG 1024 scan or mismatch guard failed: ${small_rg_output}${small_rg_error}${mismatched_rg_output}${mismatched_rg_error}")
endif()
