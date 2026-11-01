// pmbook command-line entry point.
//
//   pmbook live --asset <token_id> [--asset ...] [--record-dir DIR]
//               [--duration SEC] [--chaos-disconnect SEC] [--summary FILE]
//   pmbook replay [--speed X] [--summary FILE] [--expect FILE] FILE...
//
// Status and stats go to stderr; stdout is reserved for the PMBook stream
// (milestone 7).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/processor.hpp"
#include "app/summary.hpp"
#include "app/summary_compare.hpp"
#include "book/book_manager.hpp"
#include "core/clock.hpp"
#include "core/version.hpp"
#include "feed/feed_client.hpp"
#include "parse/parser.hpp"
#include "record/recorder.hpp"
#include "replay/replayer.hpp"

namespace {

// Signal handlers may only touch lock-free atomics (or volatile sig_atomic_t).
std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop.store(true); }

struct LiveArgs {
  std::vector<std::string> assets;
  std::string record_dir = "data/recordings";
  bool record = true;
  int duration_s = 0;  // 0 = until Ctrl-C
  int chaos_s = 0;     // 0 = off
  int stats_s = 10;
  std::string summary_path;
};

int usage() {
  std::fprintf(
      stderr,
      "usage: pmbook live --asset ID [--asset ID ...] [--record-dir DIR | --no-record]\n"
      "                   [--duration SEC] [--chaos-disconnect SEC] [--stats SEC]\n"
      "                   [--summary FILE]\n"
      "       pmbook replay [--speed X] [--stats SEC] [--summary FILE] [--expect FILE] FILE...\n"
      "                     (--speed 0 = as fast as possible, 1 = real time)\n"
      "tip: python3 tools/list_markets.py --limit 3 --ids-only\n");
  return 2;
}

std::optional<LiveArgs> parse_live(int argc, char** argv) {
  LiveArgs a;
  for (int i = 2; i < argc; ++i) {
    const std::string_view k = argv[i];
    auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (k == "--no-record") {
      a.record = false;
      continue;
    }
    if (!(v = val())) return std::nullopt;
    if (k == "--asset") {
      a.assets.emplace_back(v);
    } else if (k == "--record-dir") {
      a.record_dir = v;
    } else if (k == "--duration") {
      a.duration_s = std::atoi(v);
    } else if (k == "--chaos-disconnect") {
      a.chaos_s = std::atoi(v);
    } else if (k == "--stats") {
      a.stats_s = std::max(1, std::atoi(v));
    } else if (k == "--summary") {
      a.summary_path = v;
    } else {
      return std::nullopt;
    }
  }
  if (a.assets.empty()) return std::nullopt;
  return a;
}

int run_live(const LiveArgs& args) {
  using namespace pm;
  if (FeedClient::subscribe_message(args.assets).empty()) {
    std::fprintf(stderr, "error: token IDs must be decimal digits\n");
    return 2;
  }

  // Everything below is touched only by the frame-handling thread.
  // FeedClient guarantees callbacks from one connection never overlap with
  // the next connection's (it joins the old thread first).
  std::optional<Recorder> recorder;
  if (args.record) recorder.emplace(args.record_dir);
  FrameProcessor<FlatBook> proc({.stats_interval_s = args.stats_s, .log_resyncs = true});

  auto on_frame = [&](std::string_view raw, std::int64_t recv_ns) {
    if (recorder && !recorder->write(recv_ns, raw)) {
      std::fprintf(stderr, "error: recorder write failed (%s)\n", recorder->current_path().c_str());
    }
    proc.on_frame(raw, recv_ns);  // the same path replay uses
  };

  auto on_status = [&](FeedStatus st, std::string_view detail) {
    // Only logs. Book state is driven purely by frames (see processor.hpp).
    if (st == FeedStatus::Connected) {
      std::fprintf(stderr, "[feed] connected: %.*s\n", static_cast<int>(detail.size()),
                   detail.data());
    } else {
      std::fprintf(stderr, "[feed] reconnecting: %.*s\n", static_cast<int>(detail.size()),
                   detail.data());
    }
  };

  FeedConfig cfg;
  cfg.assets = args.assets;
  FeedClient client(cfg, on_frame, on_status);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::fprintf(stderr, "pmbook %.*s: %zu assets, recording %s\n",
               static_cast<int>(version().size()), version().data(), args.assets.size(),
               args.record ? args.record_dir.c_str() : "off");
  client.start();

  const auto t0 = std::chrono::steady_clock::now();
  auto next_chaos = t0 + std::chrono::seconds(args.chaos_s);
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto now = std::chrono::steady_clock::now();
    if (args.duration_s > 0 && now - t0 >= std::chrono::seconds(args.duration_s)) break;
    if (args.chaos_s > 0 && now >= next_chaos) {
      std::fprintf(stderr, "[chaos] forcing disconnect\n");
      client.force_disconnect();
      next_chaos = now + std::chrono::seconds(args.chaos_s);
    }
  }

  client.stop();  // joins all network threads: safe to read state below
  if (recorder) recorder->close();
  const std::string summary = proc.summary();
  std::fprintf(stderr,
               "[done] connects=%llu disconnects=%llu frames=%llu parse_errors=%llu resyncs=%llu\n",
               static_cast<unsigned long long>(client.connects()),
               static_cast<unsigned long long>(client.disconnects()),
               static_cast<unsigned long long>(client.frames()),
               static_cast<unsigned long long>(proc.parse_errors()),
               static_cast<unsigned long long>(proc.resync_events()));
  if (!args.summary_path.empty()) {
    std::ofstream(args.summary_path) << summary << "\n";
  }
  return proc.parse_errors() == 0 ? 0 : 1;
}

struct ReplayArgs {
  std::vector<std::string> files;
  double speed = 0;
  int stats_s = 0;
  std::string summary_path;
  std::string expect_path;
};

std::optional<ReplayArgs> parse_replay(int argc, char** argv) {
  ReplayArgs a;
  for (int i = 2; i < argc; ++i) {
    const std::string_view k = argv[i];
    auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (k.starts_with("--")) {
      const char* v = val();
      if (!v) return std::nullopt;
      if (k == "--speed") {
        a.speed = std::atof(v);
      } else if (k == "--stats") {
        a.stats_s = std::atoi(v);
      } else if (k == "--summary") {
        a.summary_path = v;
      } else if (k == "--expect") {
        a.expect_path = v;
      } else {
        return std::nullopt;
      }
    } else {
      a.files.emplace_back(k);
    }
  }
  if (a.files.empty() || a.speed < 0) return std::nullopt;
  return a;
}

int run_replay(const ReplayArgs& args) {
  using namespace pm;
  FrameProcessor<FlatBook> proc({.stats_interval_s = args.stats_s, .log_resyncs = true});
  std::signal(SIGINT, on_signal);
  ReplayOptions opt;
  opt.speed = args.speed;
  opt.stop = &g_stop;
  const ReplayResult r = replay_files(
      args.files, opt, [&](std::string_view raw, std::int64_t ts) { proc.on_frame(raw, ts); });

  std::fprintf(stderr, "[replay] files=%llu records=%llu feed=%.1fs wall=%.3fs (%.0f frames/s)%s\n",
               static_cast<unsigned long long>(r.files), static_cast<unsigned long long>(r.records),
               r.feed_seconds, r.wall_seconds,
               r.wall_seconds > 0 ? static_cast<double>(r.records) / r.wall_seconds : 0.0,
               r.truncated ? " [truncated file: read up to last complete line]" : "");
  if (!r.error.empty()) {
    std::fprintf(stderr, "error: %s\n", r.error.c_str());
    return 1;
  }
  proc.print_stats();
  const std::string summary = proc.summary();
  if (!args.summary_path.empty()) std::ofstream(args.summary_path) << summary << "\n";

  int rc = proc.parse_errors() == 0 ? 0 : 1;
  if (!args.expect_path.empty()) {
    std::ifstream in(args.expect_path);
    if (!in) {
      std::fprintf(stderr, "error: cannot read %s\n", args.expect_path.c_str());
      return 1;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const auto diffs = compare_summaries(ss.str(), summary);
    if (diffs.empty()) {
      std::fprintf(stderr, "[expect] MATCH: every asset's final book state hash equals %s\n",
                   args.expect_path.c_str());
    } else {
      for (const auto& d : diffs) std::fprintf(stderr, "[expect] DIFF %s\n", d.c_str());
      rc = 1;
    }
  }
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string_view cmd = argv[1];
  if (cmd == "--version") {
    std::printf("pmbook %.*s\n", static_cast<int>(pm::version().size()), pm::version().data());
    return 0;
  }
  if (cmd == "replay") {
    auto args = parse_replay(argc, argv);
    if (!args) return usage();
    return run_replay(*args);
  }
  if (cmd == "live") {
    auto args = parse_live(argc, argv);
    if (!args) return usage();
    return run_live(*args);
  }
  return usage();
}
