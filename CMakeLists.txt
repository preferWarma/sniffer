cmake_minimum_required(VERSION 3.20)

project(sniffer_core VERSION 0.1.0 LANGUAGES CXX)

option(SNIFFER_BUILD_TESTS "Build sniffer-core tests" ON)
option(SNIFFER_BUILD_BENCHMARK "Build sniffer-core benchmark" ON)
option(SNIFFER_BUILD_FUZZ_SMOKE "Build standalone fuzz smoke" ON)
option(SNIFFER_BUILD_EXAMPLES "Build sniffer-core examples" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(Arrow REQUIRED arrow)

# Homebrew's shared Arrow library can temporarily retain runtime links to an
# upgraded-away aws-sdk-cpp. Prefer the static core archive when pkg-config
# provides it; sniffer-core does not use Arrow's optional S3 integration.
find_file(SNIFFER_ARROW_STATIC_ARCHIVE
  NAMES libarrow.a
  HINTS ${Arrow_LIBRARY_DIRS}
  NO_DEFAULT_PATH
)
find_file(SNIFFER_ARROW_BUNDLED_ARCHIVE
  NAMES libarrow_bundled_dependencies.a
  HINTS ${Arrow_LIBRARY_DIRS}
  NO_DEFAULT_PATH
)

if(SNIFFER_ARROW_STATIC_ARCHIVE AND SNIFFER_ARROW_BUNDLED_ARCHIVE)
  find_package(ZLIB REQUIRED)
  find_package(BZip2 REQUIRED)
  pkg_check_modules(ArrowCompression REQUIRED IMPORTED_TARGET
    libbrotlienc
    libbrotlidec
    liblz4
    libzstd
  )
  find_library(SNIFFER_SNAPPY_LIBRARY
    NAMES snappy
    HINTS ${Arrow_LIBRARY_DIRS} /opt/homebrew/lib
    REQUIRED
  )
  add_library(sniffer_arrow STATIC IMPORTED GLOBAL)
  set_target_properties(sniffer_arrow PROPERTIES
    IMPORTED_LOCATION "${SNIFFER_ARROW_STATIC_ARCHIVE}"
    INTERFACE_INCLUDE_DIRECTORIES "${Arrow_INCLUDE_DIRS}"
    INTERFACE_COMPILE_DEFINITIONS ARROW_STATIC
  )
  target_link_libraries(sniffer_arrow INTERFACE
    "${SNIFFER_ARROW_BUNDLED_ARCHIVE}"
    "${SNIFFER_SNAPPY_LIBRARY}"
    PkgConfig::ArrowCompression
    BZip2::BZip2
    ZLIB::ZLIB
  )
else()
  pkg_check_modules(ArrowImported REQUIRED IMPORTED_TARGET arrow)
  add_library(sniffer_arrow ALIAS PkgConfig::ArrowImported)
endif()

add_library(sniffer_core
  src/codec_internal.cc
  src/format_internal.cc
  src/index_internal.cc
  src/scalar_internal.cc
  src/schema.cc
  src/segment_reader.cc
  src/segment_writer.cc
)

target_compile_features(sniffer_core PUBLIC cxx_std_20)
target_include_directories(sniffer_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(sniffer_core PUBLIC sniffer_arrow)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(sniffer_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(SNIFFER_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  add_executable(sniffer_core_tests tests/sniffer_core_test.cc)
  target_compile_features(sniffer_core_tests PRIVATE cxx_std_20)
  target_include_directories(sniffer_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(sniffer_core_tests PRIVATE sniffer_core GTest::gtest_main)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sniffer_core_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  endif()
  include(GoogleTest)
  gtest_discover_tests(sniffer_core_tests DISCOVERY_TIMEOUT 60)

  if(SNIFFER_BUILD_FUZZ_SMOKE)
    add_executable(sniffer_core_fuzz tests/sniffer_core_fuzz.cc)
    target_compile_features(sniffer_core_fuzz PRIVATE cxx_std_20)
    target_compile_definitions(sniffer_core_fuzz PRIVATE SNIFFER_FUZZ_STANDALONE)
    target_link_libraries(sniffer_core_fuzz PRIVATE sniffer_core)
    add_test(NAME sniffer_core_fuzz_smoke COMMAND sniffer_core_fuzz)
  endif()
endif()

if(SNIFFER_BUILD_BENCHMARK)
  find_package(benchmark CONFIG REQUIRED)
  set(SNIFFER_BENCHMARK_SOURCE_REVISION "unknown")
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      RESULT_VARIABLE SNIFFER_GIT_REVISION_RESULT
      OUTPUT_VARIABLE SNIFFER_BENCHMARK_SOURCE_REVISION
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(NOT SNIFFER_GIT_REVISION_RESULT EQUAL 0)
      set(SNIFFER_BENCHMARK_SOURCE_REVISION "unknown")
    else()
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE SNIFFER_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(NOT SNIFFER_GIT_STATUS STREQUAL "")
        string(APPEND SNIFFER_BENCHMARK_SOURCE_REVISION "-dirty")
      endif()
    endif()
  endif()
  configure_file(
    bench/benchmark_build_config.h.in
    generated/benchmark_build_config.h
    @ONLY
  )

  add_executable(sniffer_core_performance_benchmark bench/performance_benchmark.cc)
  target_compile_features(sniffer_core_performance_benchmark PRIVATE cxx_std_20)
  target_include_directories(
    sniffer_core_performance_benchmark PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
  )
  target_link_libraries(
    sniffer_core_performance_benchmark PRIVATE sniffer_core benchmark::benchmark
  )

  add_executable(sniffer_core_compression_benchmark bench/compression_benchmark.cc)
  target_compile_features(sniffer_core_compression_benchmark PRIVATE cxx_std_20)
  target_include_directories(
    sniffer_core_compression_benchmark PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
  )
  target_link_libraries(
    sniffer_core_compression_benchmark PRIVATE sniffer_core benchmark::benchmark
  )

  add_executable(sniffer_core_codec_benchmark bench/codec_benchmark.cc)
  target_compile_features(sniffer_core_codec_benchmark PRIVATE cxx_std_20)
  target_include_directories(
    sniffer_core_codec_benchmark
    PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}/generated"
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
  )
  target_link_libraries(
    sniffer_core_codec_benchmark PRIVATE sniffer_core benchmark::benchmark
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      sniffer_core_performance_benchmark
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion
    )
    target_compile_options(
      sniffer_core_compression_benchmark
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion
    )
    target_compile_options(
      sniffer_core_codec_benchmark
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion
    )
  endif()
  if(SNIFFER_BUILD_TESTS)
    add_test(
      NAME sniffer_core_performance_benchmark_smoke
      COMMAND sniffer_core_performance_benchmark --benchmark_dry_run
    )
    add_test(
      NAME sniffer_core_compression_benchmark_smoke
      COMMAND sniffer_core_compression_benchmark --benchmark_dry_run
    )
    add_test(
      NAME sniffer_core_codec_benchmark_smoke
      COMMAND sniffer_core_codec_benchmark --benchmark_dry_run
    )
    add_test(
      NAME sniffer_core_performance_benchmark_json_smoke
      COMMAND
        "${CMAKE_COMMAND}"
        "-DBENCHMARK_EXECUTABLE=$<TARGET_FILE:sniffer_core_performance_benchmark>"
        -DEXPECTED_PREFIX=Performance/
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate_google_benchmark_json.cmake"
    )
    add_test(
      NAME sniffer_core_compression_benchmark_json_smoke
      COMMAND
        "${CMAKE_COMMAND}"
        "-DBENCHMARK_EXECUTABLE=$<TARGET_FILE:sniffer_core_compression_benchmark>"
        -DEXPECTED_PREFIX=Compression/
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate_google_benchmark_json.cmake"
    )
    add_test(
      NAME sniffer_core_codec_benchmark_json_smoke
      COMMAND
        "${CMAKE_COMMAND}"
        "-DBENCHMARK_EXECUTABLE=$<TARGET_FILE:sniffer_core_codec_benchmark>"
        -DEXPECTED_PREFIX=Codec/
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate_google_benchmark_json.cmake"
    )
  endif()
endif()

if(SNIFFER_BUILD_EXAMPLES)
  add_executable(sniffer_core_example example/example.cpp)
  target_compile_features(sniffer_core_example PRIVATE cxx_std_20)
  target_link_libraries(sniffer_core_example PRIVATE sniffer_core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sniffer_core_example PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  endif()
endif()
