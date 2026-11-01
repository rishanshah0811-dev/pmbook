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
  test_spsc.cpp
  test_publisher.cpp
  test_histogram.cpp
  test_naive.cpp
)
target_link_libraries(pm_tests PRIVATE pmcore nlohmann_json::nlohmann_json GTest::gtest_main ZLIB::ZLIB pm_warnings)
target_compile_definitions(pm_tests PRIVATE
  PM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  PM_SAMPLE_DIR="${PROJECT_SOURCE_DIR}/data/sample")
gtest_discover_tests(pm_tests DISCOVERY_MODE PRE_TEST)

# End-to-end contract check: the C++ engine publishes the sample replay and
# the Python example consumer validates every rule in CONTRACT.md.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME Contract.EndToEndReplayToPythonConsumer
    COMMAND sh -c "\"$<TARGET_FILE:pmbook>\" replay --publish stdout \"${PROJECT_SOURCE_DIR}\"/data/sample/pm_*.jsonl.gz | \"${Python3_EXECUTABLE}\" \"${PROJECT_SOURCE_DIR}/examples/consume.py\" --validate --quiet")
endif()
