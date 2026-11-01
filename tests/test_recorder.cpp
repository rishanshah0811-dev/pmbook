#include <gtest/gtest.h>
#include <zlib.h>

#include <atomic>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "record/recorder.hpp"

using namespace pm;
namespace fs = std::filesystem;

namespace {

// Fresh, empty directory per test.
fs::path temp_dir(const std::string& name) {
  fs::path p =
      fs::temp_directory_path() / ("pmbook_test_" + name + "_" + std::to_string(::getpid()));
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

std::vector<nlohmann::json> read_gz_lines(const fs::path& file) {
  std::vector<nlohmann::json> out;
  gzFile gz = gzopen(file.string().c_str(), "rb");
  EXPECT_NE(gz, nullptr);
  std::string line;
  char buf[4096];
  while (gzgets(gz, buf, sizeof buf)) {
    line += buf;
    if (!line.empty() && line.back() == '\n') {
      out.push_back(nlohmann::json::parse(line));
      line.clear();
    }
  }
  gzclose(gz);
  EXPECT_TRUE(line.empty()) << "file did not end with a newline";
  return out;
}

}  // namespace

TEST(Recorder, FileNamesAreUtcHours) {
  EXPECT_EQ(Recorder::file_name_for(0), "pm_19700101_00.jsonl.gz");
  EXPECT_EQ(Recorder::file_name_for(1793523600), "pm_20261101_09.jsonl.gz");
  EXPECT_EQ(Recorder::file_name_for(1793523600 + 3599), "pm_20261101_09.jsonl.gz");
  EXPECT_EQ(Recorder::file_name_for(1793523600 + 3600), "pm_20261101_10.jsonl.gz");
}

TEST(Recorder, RoundTripsAwkwardFramesExactly) {
  const auto dir = temp_dir("roundtrip");
  const std::vector<std::string> frames = {
      R"({"event_type":"book","hash":"0xabc"})",
      "PONG",
      "quote \" backslash \\ newline \n tab \t cr \r",
      std::string("control \x01 \x1f end"),
      "utf8 … ✓ 日本",
      "",
  };
  std::string path;
  {
    Recorder rec(dir.string(), [] { return std::int64_t{1793523600}; });
    for (std::size_t i = 0; i < frames.size(); ++i) {
      ASSERT_TRUE(rec.write(static_cast<std::int64_t>(1000 + i), frames[i]));
    }
    path = rec.current_path();
    EXPECT_EQ(rec.records(), frames.size());
  }  // destructor closes the file
  EXPECT_EQ(fs::path(path).filename(), "pm_20261101_09.jsonl.gz");
  const auto lines = read_gz_lines(path);
  ASSERT_EQ(lines.size(), frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    EXPECT_EQ(lines[i].at("ts_recv_ns").get<std::int64_t>(), static_cast<std::int64_t>(1000 + i));
    EXPECT_EQ(lines[i].at("raw").get<std::string>(), frames[i]);
    EXPECT_EQ(lines[i].size(), 2u);  // exactly the two contract fields
  }
}

TEST(Recorder, RotatesEveryHour) {
  const auto dir = temp_dir("rotate");
  std::int64_t now = 1793523600 + 3598;
  {
    Recorder rec(dir.string(), [&now] { return now; });
    rec.write(1, "a");
    now += 1;
    rec.write(2, "b");  // still 09:59:59
    now += 1;
    rec.write(3, "c");  // 10:00:00 -> new file
  }
  const auto f09 = dir / "pm_20261101_09.jsonl.gz";
  const auto f10 = dir / "pm_20261101_10.jsonl.gz";
  ASSERT_TRUE(fs::exists(f09));
  ASSERT_TRUE(fs::exists(f10));
  EXPECT_EQ(read_gz_lines(f09).size(), 2u);
  EXPECT_EQ(read_gz_lines(f10).size(), 1u);
}

TEST(Recorder, RestartInSameHourAppends) {
  const auto dir = temp_dir("append");
  auto clock = [] { return std::int64_t{1793523600}; };
  {
    Recorder rec(dir.string(), clock);
    rec.write(1, "first run");
  }
  {
    Recorder rec(dir.string(), clock);
    rec.write(2, "second run");
  }
  const auto lines = read_gz_lines(dir / "pm_20261101_09.jsonl.gz");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1].at("raw"), "second run");
}

TEST(Recorder, DataIsReadableBeforeCloseAfterFlush) {
  // Simulates a crash: read the file while the recorder is still open.
  const auto dir = temp_dir("crash");
  Recorder rec(dir.string(), [] { return std::int64_t{1793523600}; });
  rec.write(1, "survives a crash");
  rec.flush();
  gzFile gz = gzopen(rec.current_path().c_str(), "rb");
  ASSERT_NE(gz, nullptr);
  char buf[256] = {};
  ASSERT_NE(gzgets(gz, buf, sizeof buf), nullptr);
  gzclose(gz);
  EXPECT_NE(std::string(buf).find("survives a crash"), std::string::npos);
}
