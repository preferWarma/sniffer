if(NOT DEFINED BENCHMARK_EXECUTABLE OR NOT DEFINED EXPECTED_BENCHMARK)
  message(FATAL_ERROR "benchmark JSON validation requires executable and expected benchmark")
endif()

execute_process(
  COMMAND
    "${BENCHMARK_EXECUTABLE}"
    --rows=1024
    --iterations=2
    --row-group=128
    --output-format=json
  RESULT_VARIABLE benchmark_result
  OUTPUT_VARIABLE benchmark_output
  ERROR_VARIABLE benchmark_error
)
if(NOT benchmark_result EQUAL 0)
  message(FATAL_ERROR "benchmark failed: ${benchmark_error}")
endif()

string(JSON benchmark_name GET "${benchmark_output}" benchmark)
if(NOT benchmark_name STREQUAL EXPECTED_BENCHMARK)
  message(FATAL_ERROR "unexpected benchmark name: ${benchmark_name}")
endif()

string(JSON source_revision GET "${benchmark_output}" source_revision)
string(JSON compiler GET "${benchmark_output}" compiler)
string(JSON arrow_version GET "${benchmark_output}" arrow_version)
string(JSON command_length LENGTH "${benchmark_output}" command_arguments)
if(source_revision STREQUAL "" OR compiler STREQUAL "" OR arrow_version STREQUAL "" OR
   command_length LESS 5)
  message(FATAL_ERROR "benchmark JSON is missing reproducibility metadata")
endif()

if(EXPECTED_BENCHMARK STREQUAL "performance")
  string(JSON sample_length LENGTH "${benchmark_output}" measurements 0 write_stats samples_ms)
  string(JSON p50 GET "${benchmark_output}" measurements 0 write_stats p50_ms)
elseif(EXPECTED_BENCHMARK STREQUAL "compression")
  string(JSON sample_length LENGTH "${benchmark_output}" scenarios 0 formats 0 encode_write_stats
         samples_ms)
  string(JSON p50 GET "${benchmark_output}" scenarios 0 formats 0 encode_write_stats p50_ms)
elseif(EXPECTED_BENCHMARK STREQUAL "codec")
  string(JSON sample_length LENGTH "${benchmark_output}" scenarios 0 encode_stats samples_ms)
  string(JSON p50 GET "${benchmark_output}" scenarios 0 encode_stats p50_ms)
else()
  message(FATAL_ERROR "unsupported benchmark name: ${EXPECTED_BENCHMARK}")
endif()
if(NOT sample_length EQUAL 2 OR p50 LESS_EQUAL 0)
  message(FATAL_ERROR "benchmark JSON has invalid timing statistics")
endif()
