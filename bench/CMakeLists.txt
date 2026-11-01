add_executable(pm_bench bench_parse.cpp bench_book.cpp)
target_link_libraries(pm_bench PRIVATE pmcore nlohmann_json::nlohmann_json benchmark::benchmark_main pm_warnings)
target_compile_definitions(pm_bench PRIVATE PM_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
