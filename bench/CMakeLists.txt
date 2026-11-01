add_executable(pm_bench bench_smoke.cpp)
target_link_libraries(pm_bench PRIVATE pmcore benchmark::benchmark_main pm_warnings)
