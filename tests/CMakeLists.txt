include(GoogleTest)

add_executable(pm_tests
  test_smoke.cpp
  test_fixed_point.cpp
  test_parser.cpp
  test_book.cpp
  test_book_manager.cpp
  test_feed.cpp
  test_recorder.cpp
  test_replay.cpp
)
target_link_libraries(pm_tests PRIVATE pmcore nlohmann_json::nlohmann_json GTest::gtest_main ZLIB::ZLIB pm_warnings)
target_compile_definitions(pm_tests PRIVATE
  PM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  PM_SAMPLE_DIR="${PROJECT_SOURCE_DIR}/data/sample")
gtest_discover_tests(pm_tests DISCOVERY_MODE PRE_TEST)
