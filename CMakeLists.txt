cmake_minimum_required(VERSION 3.20)

project(sniffer_core VERSION 0.1.0 LANGUAGES CXX)

option(SNIFFER_BUILD_TESTS "Build sniffer-core tests" ON)

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
  src/format_internal.cc
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
  add_executable(sniffer_core_tests tests/sniffer_core_test.cc)
  target_compile_features(sniffer_core_tests PRIVATE cxx_std_20)
  target_link_libraries(sniffer_core_tests PRIVATE sniffer_core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sniffer_core_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  endif()
  add_test(NAME sniffer_core_tests COMMAND sniffer_core_tests)
endif()
