include(GoogleTest)

add_executable(pm_tests
  test_smoke.cpp
)
target_link_libraries(pm_tests PRIVATE pmcore nlohmann_json::nlohmann_json GTest::gtest_main pm_warnings)
gtest_discover_tests(pm_tests DISCOVERY_MODE PRE_TEST)
