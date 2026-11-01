#include <gtest/gtest.h>
#include <unistd.h>
#include <zlib.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "app/processor.hpp"
#include "app/summary_compare.hpp"
#include "book/map_book.hpp"
#include "record/record_reader.hpp"
#include "record/recorder.hpp"
#include "replay/replayer.hpp"

using namespace pm;
namespace fs = std::filesystem;

namespace {

const fs::path kSample = fs::path(PM_SAMPLE_DIR);

std::vector<std::string> sample_files() {
  std::vector<std::string> out;
  for (const auto& e : fs::directory_iterator(kSample)) {
    const std::string name = e.path().filename().string();
    if (name.starts_with("pm_") && name.ends_with(".jsonl.gz")) out.push_back(e.path().string());
  }
  return out;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

template <class Book>
std::string replay_sample_summary(ReplayResult* result = nullptr) {
  typename FrameProcessor<Book>::Options opt;
  opt.log_resyncs = false;
  FrameProcessor<Book> proc(opt);
  const auto r =
      replay_files(sample_files(), ReplayOptions{},
                   [&](std::string_view raw, std::int64_t ts) { proc.on_frame(raw, ts); });
  if (result) *result = r;
  EXPECT_EQ(proc.parse_errors(), 0u);
  return proc.summary();
}

fs::path temp_dir(const std::string& name) {
  fs::path p =
      fs::temp_directory_path() / ("pmbook_replay_" + name + "_" + std::to_string(::getpid()));
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

}  // namespace

// THE milestone-5 check: replaying the recording reproduces the live run's
// final books exactly (compared by per-asset state hash) and the same counters.
TEST(Replay, SampleReplayReproducesLiveRunExactly) {
  ReplayResult r;
  const std::string actual = replay_sample_summary<FlatBook>(&r);
  const std::string expected = read_file(kSample / "live_summary.json");
  ASSERT_FALSE(expected.empty());

  const auto diffs = compare_summaries(expected, actual);
  for (const auto& d : diffs) ADD_FAILURE() << d;
  EXPECT_TRUE(diffs.empty());

  const auto e = nlohmann::json::parse(expected);
  const auto a = nlohmann::json::parse(actual);
  for (const char* k :
       {"frames", "snapshots", "deltas", "trades", "ignored_deltas", "bbo_mismatches", "crossed"}) {
    EXPECT_EQ(a.at(k), e.at(k)) << k;
  }
  EXPECT_EQ(a.at("assets").size(), 6u);
  EXPECT_EQ(r.files, 2u);
  EXPECT_EQ(r.records, 1533u);
  EXPECT_FALSE(r.truncated);
  EXPECT_TRUE(r.error.empty());
}

TEST(Replay, IsDeterministic) {
  EXPECT_EQ(replay_sample_summary<FlatBook>(), replay_sample_summary<FlatBook>());
}

TEST(Replay, FlatAndMapBooksAgreeOnRealData) {
  EXPECT_TRUE(compare_summaries(replay_sample_summary<MapBook>(), replay_sample_summary<FlatBook>())
                  .empty());
}

TEST(Replay, CompareSummariesSpotsADifference) {
  const std::string a =
      R"({"assets":{"1":{"state":"live","state_hash":"aa"},"2":{"state":"live","state_hash":"bb"}}})";
  const std::string b =
      R"({"assets":{"1":{"state":"live","state_hash":"aa"},"2":{"state":"live","state_hash":"bc"}}})";
  const std::string c = R"({"assets":{"1":{"state":"live","state_hash":"aa"}}})";
  EXPECT_TRUE(compare_summaries(a, a).empty());
  EXPECT_EQ(compare_summaries(a, b).size(), 1u);
  EXPECT_EQ(compare_summaries(a, c).size(), 1u);  // missing asset
  EXPECT_EQ(compare_summaries(c, a).size(), 1u);  // unexpected asset
  EXPECT_FALSE(compare_summaries("nope", a).empty());
}

namespace {
// Writes a recording with frames at the given recorded timestamps (ns).
std::string make_recording(const fs::path& dir, const std::vector<std::int64_t>& ts) {
  Recorder rec(dir.string(), [] { return std::int64_t{1793523600}; });
  for (std::size_t i = 0; i < ts.size(); ++i) rec.write(ts[i], "frame" + std::to_string(i));
  return rec.current_path();
}
double replay_wall(const std::string& file, double speed,
                   std::chrono::milliseconds max_gap = std::chrono::seconds(60)) {
  ReplayOptions opt;
  opt.speed = speed;
  opt.max_gap = max_gap;
  int n = 0;
  const auto r = replay_files({file}, opt, [&](std::string_view, std::int64_t) { ++n; });
  EXPECT_EQ(n, static_cast<int>(r.records));
  return r.wall_seconds;
}
}  // namespace

TEST(Replay, RealSpeedKeepsRecordedGaps) {
  const auto dir = temp_dir("pacing");
  // Four frames, 100 ms apart -> 300 ms of recorded time.
  const auto file =
      make_recording(dir, {1'000'000'000, 1'100'000'000, 1'200'000'000, 1'300'000'000});
  const double fast = replay_wall(file, 0);
  const double real = replay_wall(file, 1.0);
  const double x10 = replay_wall(file, 10.0);
  EXPECT_LT(fast, 0.05);
  EXPECT_GE(real, 0.29);
  EXPECT_LT(real, 0.60);
  EXPECT_GE(x10, 0.029);
  EXPECT_LT(x10, 0.20);
}

TEST(Replay, ClampsBackwardsAndHugeGaps) {
  const auto dir = temp_dir("clamp");
  // Backwards jump (e.g. a restart after reboot), then a one-hour pause.
  const auto file = make_recording(dir, {5'000'000'000, 1'000'000'000, 3'601'000'000'000});
  const double wall = replay_wall(file, 1.0, std::chrono::milliseconds(50));
  EXPECT_LT(wall, 0.3);  // not 1 hour
}

TEST(RecordReader, ReadsUpToTruncationWithoutError) {
  const auto dir = temp_dir("trunc");
  std::vector<std::int64_t> ts(2000);
  for (std::size_t i = 0; i < ts.size(); ++i) ts[i] = static_cast<std::int64_t>(i);
  const auto file = make_recording(dir, ts);
  // Chop the compressed file at 60%, like a crash mid-write.
  const auto size = fs::file_size(file);
  fs::resize_file(file, size * 6 / 10);

  RecordReader reader;
  ASSERT_TRUE(reader.open(file));
  Record r;
  std::int64_t expected_ts = 0;
  int n = 0;
  while (reader.next(r)) {
    ASSERT_EQ(r.ts_recv_ns, expected_ts++);
    ASSERT_EQ(r.raw, "frame" + std::to_string(n));
    ++n;
  }
  EXPECT_GT(n, 0);
  EXPECT_LT(n, 2000);
  EXPECT_TRUE(reader.truncated());
  EXPECT_TRUE(reader.error().empty());
}

TEST(RecordReader, ReportsMalformedLines) {
  const auto dir = temp_dir("malformed");
  const auto file = (dir / "bad.jsonl.gz").string();
  gzFile gz = gzopen(file.c_str(), "wb");
  const std::string content = "{\"ts_recv_ns\":1,\"raw\":\"ok\"}\nnot json\n";
  gzwrite(gz, content.data(), static_cast<unsigned>(content.size()));
  gzclose(gz);

  RecordReader reader;
  ASSERT_TRUE(reader.open(file));
  Record r;
  ASSERT_TRUE(reader.next(r));
  EXPECT_EQ(r.raw, "ok");
  EXPECT_FALSE(reader.next(r));
  EXPECT_NE(reader.error().find("line 2"), std::string::npos);
}

TEST(RecordReader, MissingFileIsAnError) {
  RecordReader reader;
  EXPECT_FALSE(reader.open("/nonexistent/pm_x.jsonl.gz"));
  const auto r = replay_files({"/nonexistent/pm_x.jsonl.gz"}, ReplayOptions{},
                              [](std::string_view, std::int64_t) {});
  EXPECT_FALSE(r.error.empty());
}
