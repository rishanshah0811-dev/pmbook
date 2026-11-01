cmake_minimum_required(VERSION 3.24)

# On macOS, prefer Homebrew LLVM. Xcode's bundled ASan/TSan runtimes can lag
# behind the OS and crash at startup (seen with Xcode 16.0 on macOS 26).
# Must run before project(), which locks in the compiler.
include(cmake/MacLLVM.cmake)

project(pmbook VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PM_BUILD_TESTS "Build unit tests" ON)
option(PM_BUILD_BENCH "Build benchmarks" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Sanitizers)
include(Warnings)
include(Dependencies)

# Core library: everything except main() lives here so tests and benches link it.
add_library(pmcore
  src/core/version.cpp
  src/parse/fixed_point.cpp
  src/parse/parser.cpp
  src/book/flat_book.cpp
)
target_include_directories(pmcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pmcore PUBLIC simdjson::simdjson PRIVATE pm_warnings)

add_executable(pmbook src/main.cpp)
target_link_libraries(pmbook PRIVATE pmcore ixwebsocket pm_warnings)

if(PM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(PM_BUILD_BENCH)
  add_subdirectory(bench)
endif()
