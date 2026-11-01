#pragma once
// The one and only frame-processing path: raw frame -> parse -> books.
//
// Live mode and replay mode both call FrameProcessor::on_frame, so a replay
// exercises exactly the code that ran live. Processing is a pure function of
// the frame sequence. It never looks at the wall clock or connection events,
// which is what makes replays bit-for-bit reproducible.
//
// (Reconnects need no special handling here: after every (re)subscribe the
// server's first frame carries fresh snapshots for all assets, which replace
// the books. Verified on the recorded sample. If a delta ever arrived first,
// the best bid/ask check would flag the asset for resync.)

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "app/summary.hpp"
#include "book/book_manager.hpp"
#include "parse/parser.hpp"

namespace pm {

// Book and ParserT are template parameters so the naive baseline
// (MapBook + NaiveParser) runs through exactly the same code.
template <class Book, class ParserT = Parser>
class FrameProcessor {
 public:
  struct Options {
    int stats_interval_s = 0;  // print stats to stderr every N seconds of feed time (0 = off)
    bool log_resyncs = true;
  };

  explicit FrameProcessor(Options opt = {}) : opt_(opt) {}

  void on_frame(std::string_view raw, std::int64_t recv_ns) {
    ++frames_;
    events_.clear();  // a frame that fails to parse must not re-publish old events
    const ParseStatus st = parser_.parse(raw, frame_);
    if (st == ParseStatus::Error) {
      ++parse_errors_;
      std::fprintf(stderr, "parse error: %s: %.120s\n", parser_.last_error(),
                   std::string(raw).c_str());
      return;
    }
    mgr_.apply(frame_, events_);
    for (const auto& e : events_) {
      if (e.kind == EventKind::ResyncNeeded) {
        ++resync_events_;
        if (opt_.log_resyncs) {
          std::fprintf(stderr, "resync needed: %.12s...: %s\n", e.asset->asset_id.c_str(),
                       e.reason);
        }
      }
    }
    if (opt_.stats_interval_s > 0 && recv_ns >= next_stats_ns_) {
      next_stats_ns_ = recv_ns + std::int64_t{opt_.stats_interval_s} * 1'000'000'000;
      print_stats();
    }
  }

  // Events produced by the most recent frame (valid until the next frame).
  const std::vector<BookEvent<Book>>& events() const noexcept { return events_; }
  const ParsedFrame& frame() const noexcept { return frame_; }
  const BookManager<Book>& books() const noexcept { return mgr_; }
  std::uint64_t frames() const noexcept { return frames_; }
  std::uint64_t parse_errors() const noexcept { return parse_errors_; }
  std::uint64_t resync_events() const noexcept { return resync_events_; }
  std::string summary() const { return summary_json(mgr_, frames_); }

  void print_stats() const {
    const auto& s = mgr_.stats();
    std::fprintf(
        stderr,
        "[stats] frames=%llu snapshots=%llu deltas=%llu trades=%llu bbo_mismatch=%llu "
        "crossed=%llu parse_err=%llu\n",
        static_cast<unsigned long long>(frames_), static_cast<unsigned long long>(s.snapshots),
        static_cast<unsigned long long>(s.deltas), static_cast<unsigned long long>(s.trades),
        static_cast<unsigned long long>(s.bbo_mismatches),
        static_cast<unsigned long long>(s.crossed), static_cast<unsigned long long>(parse_errors_));
    for (const auto& [id, a] : mgr_.assets()) {
      std::fprintf(stderr, "        %.12s... %-6s bid %s  ask %s  levels %zu/%zu\n", id.c_str(),
                   a.state == AssetState::Live ? "live" : "resync",
                   a.book->best_bid() == kNoPrice ? "-" : format_price(a.book->best_bid()).c_str(),
                   a.book->best_ask() == kNoPrice ? "-" : format_price(a.book->best_ask()).c_str(),
                   a.book->level_count(Side::Buy), a.book->level_count(Side::Sell));
    }
  }

 private:
  Options opt_;
  ParserT parser_;
  ParsedFrame frame_;
  BookManager<Book> mgr_;
  std::vector<BookEvent<Book>> events_;
  std::uint64_t frames_ = 0;
  std::uint64_t parse_errors_ = 0;
  std::uint64_t resync_events_ = 0;
  std::int64_t next_stats_ns_ = 0;
};

}  // namespace pm
