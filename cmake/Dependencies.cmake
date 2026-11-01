# All third-party code is downloaded and built by FetchContent, so one
# `cmake --preset` command is enough. Versions are pinned for reproducibility.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# simdjson: SIMD-accelerated JSON parser for the hot path.
FetchContent_Declare(simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG v4.6.11
  GIT_SHALLOW TRUE
  SYSTEM)
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)

# nlohmann/json: easy but slow JSON, used only by the "naive" baseline.
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
  SYSTEM)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

# IXWebSocket: small WebSocket + TLS client. TLS via OpenSSL so macOS and
# Linux use the same code path.
if(APPLE AND NOT OPENSSL_ROOT_DIR)
  execute_process(COMMAND brew --prefix openssl@3
    OUTPUT_VARIABLE _brew_ssl OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_brew_ssl)
    set(OPENSSL_ROOT_DIR "${_brew_ssl}")
  endif()
endif()
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
FetchContent_Declare(ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v11.4.6
  GIT_SHALLOW TRUE
  SYSTEM)
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
set(USE_ZLIB ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(simdjson nlohmann_json ixwebsocket)

if(PM_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
    SYSTEM)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

if(PM_BUILD_BENCH)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.5
    GIT_SHALLOW TRUE
    SYSTEM)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()
