// pmbook command-line entry point.
//
//   pmbook live --asset <token_id> [--asset ...] [--record-dir DIR]
//               [--duration SEC] [--chaos-disconnect SEC] [--summary FILE]
//               [--wait spin|hybrid]
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
#include <memory>
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
#include "core/histogram.hpp"
#include "core/machine.hpp"
#include "core/version.hpp"
#include "feed/feed_client.hpp"
#include "parse/parser.hpp"
#include "publish/publisher.hpp"
#include "publish/sink.hpp"
#include "publish/tcp_sink.hpp"
#include "queue/frame_pipeline.hpp"
#include "record/recorder.hpp"
#include "replay/replayer.hpp"

namespace {

// Signal handlers may only touch lock-free atomics (or volatile sig_atomic_t).
std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop.store(true); }

// Output options shared by live and replay (see CONTRACT.md).
struct PublishArgs {
  std::string mode;  // "stdout" | "tcp" | "none"
  std::uint16_t port = 9100;
  std::size_t depth = 10;

  // Handles --publish / --publish-port / --depth. Returns false if `k` isn't
  // one of ours; sets `bad` if the value is invalid.
  bool parse(std::string_view k, const char* v, bool& bad) {
    if (k == "--publish") {
      mode = v;
      bad = !(mode == "stdout" || mode == "tcp" || mode == "none");
    } else if (k == "--publish-port") {
      const int p = std::atoi(v);
      bad = p <= 0 || p > 65535;
      port = static_cast<std::uint16_t>(p);
    } else if (k == "--depth") {
      const int d = std::atoi(v);
      bad = d <= 0 || d > 1000;
      depth = static_cast<std::size_t>(d);
    } else {
      return false;
    }
    return true;
  }
};

// Builds the sink for the chosen mode; nullptr for "none" or on error.
std::unique_ptr<pm::Sink> make_sink(const PublishArgs& p, bool& error) {
  error = false;
  if (p.mode == "stdout") return std::make_unique<pm::StdoutSink>();
  if (p.mode == "tcp") {
    auto tcp = std::make_unique<pm::TcpSink>(p.port);
    if (!tcp->start()) {
      std::fprintf(stderr, "error: cannot listen on %s\n", tcp->error().c_str());
      error = true;
      return nullptr;
    }
    std::fprintf(stderr, "[publish] PMBook v1 on tcp://127.0.0.1:%u\n", tcp->port());
    return tcp;
  }
  return nullptr;
}

struct LiveArgs {
  std::vector<std::string> assets;
  std::string record_dir = "data/recordings";
  bool record = true;
  int duration_s = 0;  // 0 = until Ctrl-C
  int chaos_s = 0;     // 0 = off
  int stats_s = 10;
  std::string summary_path;
  pm::WaitMode wait = pm::WaitMode::Hybrid;
  PublishArgs publish{.mode = "stdout"};
  std::string latency_out;  // write latency histograms as JSON here
};

int usage() {
  std::fprintf(
      stderr,
      "usage: pmbook live --asset ID [--asset ID ...] [--record-dir DIR | --no-record]\n"
      "                   [--duration SEC] [--chaos-disconnect SEC] [--stats SEC]\n"
      "                   [--summary FILE] [--wait spin|hybrid]\n"
      "                   [--publish stdout|tcp|none] [--publish-port N] [--depth N]\n"
      "                   [--latency-out FILE]\n"
      "       pmbook replay [--speed X] [--stats SEC] [--summary FILE] [--expect FILE]\n"
      "                     [--publish stdout|tcp|none] [--publish-port N] [--depth N] FILE...\n"
      "                     (--speed 0 = as fast as possible, 1 = real time)\n"
      "  live publishes to stdout by default; replay publishes nothing by default.\n"
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
    bool bad = false;
    if (a.publish.parse(k, v, bad)) {
      if (bad) return std::nullopt;
    } else if (k == "--asset") {
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
    } else if (k == "--latency-out") {
      a.latency_out = v;
    } else if (k == "--wait") {
      if (std::string_view(v) == "spin") {
        a.wait = pm::WaitMode::Spin;
      } else if (std::string_view(v) == "hybrid") {
        a.wait = pm::WaitMode::Hybrid;
      } else {
        return std::nullopt;
      }
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

  // Threads: the network thread (inside FeedClient) only timestamps each
  // frame and pushes it into a lock-free SPSC queue. The processing thread
  // (inside FramePipeline) pops frames, updates books, then records them.
  // Recording happens after processing so gzip never delays a book update.
  // proc and recorder are touched only by the processing thread.
  std::optional<Recorder> recorder;
  if (args.record) recorder.emplace(args.record_dir);
  FrameProcessor<FlatBook> proc({.stats_interval_s = args.stats_s, .log_resyncs = true});
  bool sink_error = false;
  std::unique_ptr<Sink> sink = make_sink(args.publish, sink_error);
  if (sink_error) return 1;
  std::optional<Publisher> publisher;
  if (sink) publisher.emplace(*sink, args.publish.depth);

  // Latency, measured on the processing thread (see learn/09):
  //   internal = recv_ns (stamped on the network thread) -> books updated and
  //              messages handed to the sink. Includes queueing and wake-up.
  //   service  = just the processing call (parse + book + publish).
  // Only frames that produced a published message are counted (PONGs and
  // ignored frames would flatter the numbers).
  LatencyHistogram internal_hist, service_hist;
  std::uint64_t unpublished_frames = 0;

  FramePipeline pipeline(
      4096, args.wait,
      [&](std::string_view raw, std::int64_t recv_ns) {
        const std::int64_t start = steady_ns();
        proc.on_frame(raw, recv_ns);  // the same path replay uses
        if (publisher) publisher->on_events(proc.events(), recv_ns);
        const std::int64_t done = steady_ns();
        if (!proc.events().empty()) {
          internal_hist.record(done - recv_ns);
          service_hist.record(done - start);
        } else {
          ++unpublished_frames;
        }
        if (recorder && !recorder->write(recv_ns, raw)) {
          std::fprintf(stderr, "error: recorder write failed (%s)\n",
                       recorder->current_path().c_str());
        }
      },
      [&](SlotKind kind, std::string_view detail) {
        if (publisher)
          publisher->publish_status(kind == SlotKind::Connected ? "connected" : "reconnecting",
                                    detail);
      });
  pipeline.start();

  // Runs on the network thread. FeedClient guarantees frames from one
  // connection never overlap with the next one's, so there is exactly one
  // producer at a time, as the SPSC queue requires.
  auto on_frame = [&](std::string_view raw, std::int64_t recv_ns) { pipeline.push(raw, recv_ns); };

  auto on_status = [&](FeedStatus st, std::string_view detail) {
    // Logs, and forwards to the publisher via the queue (in order with frames).
    // Book state is driven purely by frames (see processor.hpp).
    pipeline.push_status(st == FeedStatus::Connected ? SlotKind::Connected : SlotKind::Reconnecting,
                         detail);
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
  std::signal(SIGPIPE, SIG_IGN);  // a consumer closing our stdout must not kill the engine
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

  client.stop();    // joins network threads: no more pushes
  pipeline.stop();  // drains the queue, joins the processing thread
  if (recorder) recorder->close();
  const std::string summary = proc.summary();
  std::fprintf(stderr,
               "[done] connects=%llu disconnects=%llu frames=%llu parse_errors=%llu resyncs=%llu "
               "queue_full_waits=%llu\n",
               static_cast<unsigned long long>(client.connects()),
               static_cast<unsigned long long>(client.disconnects()),
               static_cast<unsigned long long>(client.frames()),
               static_cast<unsigned long long>(proc.parse_errors()),
               static_cast<unsigned long long>(proc.resync_events()),
               static_cast<unsigned long long>(pipeline.full_waits()));
  if (!args.summary_path.empty()) {
    std::ofstream(args.summary_path) << summary << "\n";
  }
  std::fprintf(stderr,
               "[latency] internal recv->published (n=%llu): p50 %.1f us  p99 %.1f us  p99.9 %.1f "
               "us  max %.1f "
               "us | service p50 %.2f us\n",
               static_cast<unsigned long long>(internal_hist.count()),
               static_cast<double>(internal_hist.percentile(0.5)) / 1e3,
               static_cast<double>(internal_hist.percentile(0.99)) / 1e3,
               static_cast<double>(internal_hist.percentile(0.999)) / 1e3,
               static_cast<double>(internal_hist.max()) / 1e3,
               static_cast<double>(service_hist.percentile(0.5)) / 1e3);
  if (!args.latency_out.empty()) {
    std::ofstream out(args.latency_out);
    out << "{\"mode\":\"live\",\"wait\":\"" << (args.wait == WaitMode::Spin ? "spin" : "hybrid")
        << "\",\"publish\":\"" << args.publish.mode << "\",\"machine\":\"" << machine_description()
        << "\",\"build\":\"" << build_description() << "\",\"assets\":" << args.assets.size()
        << ",\"duration_s\":" << args.duration_s << ",\"frames\":" << client.frames()
        << ",\"frames_without_output\":" << unpublished_frames
        << ",\"internal_latency\":" << internal_hist.to_json()
        << ",\"service_time\":" << service_hist.to_json() << "}\n";
  }
  return proc.parse_errors() == 0 ? 0 : 1;
}

struct ReplayArgs {
  std::vector<std::string> files;
  double speed = 0;
  int stats_s = 0;
  std::string summary_path;
  std::string expect_path;
  PublishArgs publish{.mode = "none"};
};

std::optional<ReplayArgs> parse_replay(int argc, char** argv) {
  ReplayArgs a;
  for (int i = 2; i < argc; ++i) {
    const std::string_view k = argv[i];
    auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (k.starts_with("--")) {
      const char* v = val();
      if (!v) return std::nullopt;
      bool bad = false;
      if (a.publish.parse(k, v, bad)) {
        if (bad) return std::nullopt;
      } else if (k == "--speed") {
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
  bool sink_error = false;
  std::unique_ptr<Sink> sink = make_sink(args.publish, sink_error);
  if (sink_error) return 1;
  std::optional<Publisher> publisher;
  if (sink) publisher.emplace(*sink, args.publish.depth);
  std::signal(SIGINT, on_signal);
  std::signal(SIGPIPE, SIG_IGN);
  ReplayOptions opt;
  opt.speed = args.speed;
  opt.stop = &g_stop;
  // In replay, ts_recv_ns in published messages is the RECORDED receive time
  // (it identifies the frame); ts_pub_ns is the current clock.
  const ReplayResult r = replay_files(args.files, opt, [&](std::string_view raw, std::int64_t ts) {
    proc.on_frame(raw, ts);
    if (publisher) publisher->on_events(proc.events(), ts);
  });

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
