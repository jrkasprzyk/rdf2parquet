cmake_minimum_required(VERSION 3.25)
project(rdf2parquet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive-)
  # CMAKE_CXX_FLAGS* are one-shot cache entries seeded during compiler
  # detection. Configuring outside a Developer prompt leaves cl.exe unable to
  # find the STL, detection fails, and CMake caches them *empty* — so the build
  # silently loses /EHsc (no unwind code, leaks on every throw) and /O2. The
  # vcpkg toolchain hides this by pinning CMAKE_CXX_COMPILER itself, letting the
  # configure succeed. Later configures reuse the cache, so this never heals on
  # its own: the fix is to delete CMakeCache.txt and reconfigure from a
  # Developer PowerShell for VS 2022.
  if(NOT CMAKE_CXX_FLAGS MATCHES "/EHsc")
    message(FATAL_ERROR
      "MSVC default flags are missing from the cache (no /EHsc). This build "
      "directory was configured without the MSVC developer environment. "
      "Delete ${CMAKE_BINARY_DIR}/CMakeCache.txt and ${CMAKE_BINARY_DIR}/CMakeFiles, "
      "then reconfigure from a Developer PowerShell for VS 2022.")
  endif()
else()
  add_compile_options(-Wall -Wextra)
endif()

find_package(Arrow CONFIG REQUIRED)
find_package(Parquet CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

# rdf2parquet_lib: the parser and Parquet read/write core. Knows nothing about
# the CLI — main.cpp is the only translation unit that wires it to argv.
add_library(rdf2parquet_lib STATIC
  src/rdf_parser.cpp
  src/parquet_write_long.cpp
  src/parquet_write_wide.cpp
  src/parquet_read.cpp
)
target_include_directories(rdf2parquet_lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# vcpkg's default triplet is static on Linux but dynamic on Windows, so only
# one of Arrow::arrow_static / Arrow::arrow_shared actually exists depending
# on platform (same for Parquet) — pick whichever the triplet produced rather
# than hardcoding one and breaking the other.
if(TARGET Arrow::arrow_static)
  set(RDF2PARQUET_ARROW_TARGET Arrow::arrow_static)
else()
  set(RDF2PARQUET_ARROW_TARGET Arrow::arrow_shared)
endif()
if(TARGET Parquet::parquet_static)
  set(RDF2PARQUET_PARQUET_TARGET Parquet::parquet_static)
else()
  set(RDF2PARQUET_PARQUET_TARGET Parquet::parquet_shared)
endif()

target_link_libraries(rdf2parquet_lib PUBLIC
  ${RDF2PARQUET_ARROW_TARGET}
  ${RDF2PARQUET_PARQUET_TARGET}
)
target_compile_definitions(rdf2parquet_lib PUBLIC
  RDF2PARQUET_VERSION="${PROJECT_VERSION}"
)

add_executable(rdf2parquet src/main.cpp)
target_link_libraries(rdf2parquet PRIVATE rdf2parquet_lib CLI11::CLI11)

enable_testing()
option(RDF2PARQUET_BUILD_TESTS "Build the rdf2parquet test suite" ON)
if(RDF2PARQUET_BUILD_TESTS)
  find_package(Catch2 3 CONFIG REQUIRED)

  add_executable(rdf2parquet_tests
    tests/test_rdf_parser.cpp
    tests/test_write_long.cpp
    tests/test_write_wide.cpp
    tests/test_cli.cpp
  )
  target_link_libraries(rdf2parquet_tests PRIVATE rdf2parquet_lib Catch2::Catch2WithMain)
  target_compile_definitions(rdf2parquet_tests PRIVATE
    RDF2PARQUET_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    RDF2PARQUET_CLI_PATH="$<TARGET_FILE:rdf2parquet>"
  )

  include(CTest)
  include(Catch)
  catch_discover_tests(rdf2parquet_tests)
endif()
