if(NOT DEFINED BENCHMARK_EXECUTABLE OR NOT DEFINED EXPECTED_PREFIX)
  message(FATAL_ERROR "benchmark JSON validation requires executable and expected prefix")
endif()

execute_process(
  COMMAND
    "${BENCHMARK_EXECUTABLE}"
    --benchmark_dry_run
    --benchmark_format=json
  RESULT_VARIABLE benchmark_result
  OUTPUT_VARIABLE benchmark_output
  ERROR_VARIABLE benchmark_error
)
if(NOT benchmark_result EQUAL 0)
  message(FATAL_ERROR "benchmark failed: ${benchmark_error}")
endif()

string(JSON benchmark_count LENGTH "${benchmark_output}" benchmarks)
string(JSON executable GET "${benchmark_output}" context executable)
string(JSON source_revision GET "${benchmark_output}" context source_revision)
string(JSON compiler GET "${benchmark_output}" context compiler)
string(JSON arrow_version GET "${benchmark_output}" context arrow_version)
if(benchmark_count LESS 1 OR executable STREQUAL "" OR source_revision STREQUAL "" OR
   compiler STREQUAL "" OR arrow_version STREQUAL "")
  message(FATAL_ERROR "Google Benchmark JSON is missing results or reproducibility metadata")
endif()

set(found_expected_prefix FALSE)
math(EXPR benchmark_last "${benchmark_count} - 1")
foreach(index RANGE 0 ${benchmark_last})
  string(JSON benchmark_name GET "${benchmark_output}" benchmarks ${index} name)
  string(FIND "${benchmark_name}" "${EXPECTED_PREFIX}" prefix_position)
  if(prefix_position EQUAL 0)
    string(JSON real_time GET "${benchmark_output}" benchmarks ${index} real_time)
    string(JSON time_unit GET "${benchmark_output}" benchmarks ${index} time_unit)
    if(real_time LESS 0 OR time_unit STREQUAL "")
      message(FATAL_ERROR "Google Benchmark JSON contains an invalid timing result")
    endif()
    set(found_expected_prefix TRUE)
    break()
  endif()
endforeach()
if(NOT found_expected_prefix)
  message(FATAL_ERROR "Google Benchmark JSON does not contain prefix ${EXPECTED_PREFIX}")
endif()
