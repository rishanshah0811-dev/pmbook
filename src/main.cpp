// pmbook command-line entry point.
//
//   pmbook live --asset <token_id> [--asset ...] [--record-dir DIR]
//               [--duration SEC] [--chaos-disconnect SEC] [--summary FILE]
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
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/summary.hpp"
#include "book/book_manager.hpp"
#include "core/clock.hpp"
#include "core/version.hpp"
#include "feed/feed_client.hpp"
#include "parse/parser.hpp"
#include "record/recorder.hpp"

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
  std::fprintf(stderr,
               "usage: pmbook live --asset ID [--asset ID ...] [--record-dir DIR | --no-record]\n"
               "                   [--duration SEC] [--chaos-disconnect SEC] [--stats SEC]\n"
               "                   [--summary FILE]\n"
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

  // Everything below is touched only by the frame-handling thread, except
  // where noted. FeedClient guarantees callbacks from one connection never
  // overlap with the next connection's (it joins the old thread first).
  std::optional<Recorder> recorder;
  if (args.record) recorder.emplace(args.record_dir);
  Parser parser;
  ParsedFrame frame;
  BookManager<FlatBook> mgr;
  std::vector<BookManager<FlatBook>::Event> events;
  std::uint64_t parse_errors = 0;
  std::uint64_t resync_events = 0;
  std::int64_t next_stats_ns = steady_ns() + std::int64_t{args.stats_s} * 1'000'000'000;
  bool reconnected = false;

  auto on_frame = [&](std::string_view raw, std::int64_t recv_ns) {
    if (recorder && !recorder->write(recv_ns, raw)) {
      std::fprintf(stderr, "error: recorder write failed (%s)\n", recorder->current_path().c_str());
    }
    if (reconnected) {
      // We may have missed messages while disconnected: distrust every book
      // until the fresh snapshots (sent on subscribe) arrive.
      mgr.invalidate_all();
      reconnected = false;
    }
    const ParseStatus st = parser.parse(raw, frame);
    if (st == ParseStatus::Error) {
      ++parse_errors;
      std::fprintf(stderr, "parse error: %s: %.120s\n", parser.last_error(),
                   std::string(raw).c_str());
      return;
    }
    events.clear();
    mgr.apply(frame, events);
    for (const auto& e : events) {
      if (e.kind == EventKind::ResyncNeeded) {
        ++resync_events;
        std::fprintf(stderr, "resync needed: %.12s...: %s\n", e.asset->asset_id.c_str(), e.reason);
      }
    }
    if (recv_ns >= next_stats_ns) {
      next_stats_ns = recv_ns + std::int64_t{args.stats_s} * 1'000'000'000;
      const auto& s = mgr.stats();
      std::fprintf(
          stderr,
          "[stats] snapshots=%llu deltas=%llu trades=%llu bbo_mismatch=%llu crossed=%llu "
          "parse_err=%llu recorded=%llu\n",
          static_cast<unsigned long long>(s.snapshots), static_cast<unsigned long long>(s.deltas),
          static_cast<unsigned long long>(s.trades),
          static_cast<unsigned long long>(s.bbo_mismatches),
          static_cast<unsigned long long>(s.crossed), static_cast<unsigned long long>(parse_errors),
          static_cast<unsigned long long>(recorder ? recorder->records() : 0));
      for (const auto& [id, a] : mgr.assets()) {
        std::fprintf(
            stderr, "        %.12s... %-6s bid %s  ask %s  levels %zu/%zu\n", id.c_str(),
            a.state == AssetState::Live ? "live" : "resync",
            a.book->best_bid() == kNoPrice ? "-" : format_price(a.book->best_bid()).c_str(),
            a.book->best_ask() == kNoPrice ? "-" : format_price(a.book->best_ask()).c_str(),
            a.book->level_count(Side::Buy), a.book->level_count(Side::Sell));
      }
    }
  };

  auto on_status = [&](FeedStatus st, std::string_view detail) {
    // Connected runs on the connection's own thread, before any frames.
    // Reconnecting runs on the supervisor thread while no connection exists.
    if (st == FeedStatus::Connected) {
      std::fprintf(stderr, "[feed] connected: %.*s\n", static_cast<int>(detail.size()),
                   detail.data());
    } else {
      reconnected = true;
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
  const std::string summary = summary_json(mgr, client.frames());
  std::fprintf(stderr,
               "[done] connects=%llu disconnects=%llu frames=%llu parse_errors=%llu resyncs=%llu\n",
               static_cast<unsigned long long>(client.connects()),
               static_cast<unsigned long long>(client.disconnects()),
               static_cast<unsigned long long>(client.frames()),
               static_cast<unsigned long long>(parse_errors),
               static_cast<unsigned long long>(resync_events));
  if (!args.summary_path.empty()) {
    std::ofstream(args.summary_path) << summary << "\n";
  }
  return parse_errors == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string_view cmd = argv[1];
  if (cmd == "--version") {
    std::printf("pmbook %.*s\n", static_cast<int>(pm::version().size()), pm::version().data());
    return 0;
  }
  if (cmd == "live") {
    auto args = parse_live(argc, argv);
    if (!args) return usage();
    return run_live(*args);
  }
  return usage();
}
