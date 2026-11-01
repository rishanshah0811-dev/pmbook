// End-to-end latency and throughput benchmark on recorded data:
// naive (nlohmann + std::map + mutex queue) vs optimised (simdjson + flat
// array + SPSC queue). Both run the same FrameProcessor and Publisher code.
//
//   latency mode (default): a producer thread plays the recording at --speed
//   times real time (recorded gaps / speed), --loops times, and stamps each
//   frame just before handing it to the queue, like the network thread does
//   live. The processing thread parses, updates books, formats the PMBook
//   lines into a sink that discards bytes, then records
//       internal = now - stamp        (queue + wake-up + processing)
//       service  = now - handler start (processing only)
//   for every frame that produced output.
//
//   --throughput: no threads, no pacing; process every frame back to back
//   and report frames/s (processing capacity).
//
// The recording is loaded into memory first, so disk and gzip aren't timed.
//
//   pm_latency_bench --variant optimized|naive [--wait spin|hybrid]
//                    [--speed 1000] [--loops 200] [--throughput]
//                    [--expect live_summary.json] --out results/x.json FILES...

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/processor.hpp"
#include "app/summary_compare.hpp"
#include "book/map_book.hpp"
#include "core/clock.hpp"
#include "core/histogram.hpp"
#include "core/machine.hpp"
#include "naive/naive_parser.hpp"
#include "publish/publisher.hpp"
#include "queue/frame_pipeline.hpp"
#include "queue/mutex_queue.hpp"
#include "record/record_reader.hpp"

namespace {

using namespace pm;

struct Frame {
  std::int64_t ts;  // recorded receive time
  std::string raw;
};

struct Args {
  std::string variant = "optimized";
  WaitMode wait = WaitMode::Spin;
  double speed = 1000;
  int loops = 200;
  bool throughput = false;
  bool stdout_sink = false;  // write lines to stdout (a pipe) instead of discarding
  std::string expect, out;
  std::vector<std::string> files;
};

std::vector<Frame> load(std::vector<std::string> files) {
  std::sort(files.begin(), files.end());
  std::vector<Frame> out;
  RecordReader r;
  Record rec;
  for (const auto& f : files) {
    if (!r.open(f)) {
      std::fprintf(stderr, "cannot open %s\n", f.c_str());
      std::exit(1);
    }
    while (r.next(rec)) out.push_back({rec.ts_recv_ns, std::string(rec.raw)});
  }
  return out;
}

// The naive pipeline: a std::mutex queue of owned strings (one allocation per
// frame), same wait strategy as FramePipeline so only the queue differs.
class MutexPipeline {
 public:
  using Handler = std::function<void(std::string_view, std::int64_t)>;
  MutexPipeline(std::size_t cap, WaitMode mode, Handler h)
      : q_(cap), mode_(mode), h_(std::move(h)) {}
  void start() {
    running_ = true;
    t_ = std::thread([this] { run(); });
  }
  void push(std::string_view raw, std::int64_t ns) {
    FrameSlot s{SlotKind::Frame, ns, std::string(raw)};
    while (!q_.try_push(std::move(s))) std::this_thread::yield();
  }
  void stop() {
    running_ = false;
    t_.join();
  }

 private:
  void run() {
    FrameSlot s;
    auto idle_since = std::chrono::steady_clock::now();
    bool idle = false;
    while (true) {
      if (q_.try_pop(s)) {
        h_(s.data, s.recv_ns);
        idle = false;
        continue;
      }
      if (!running_.load()) {
        if (!q_.try_pop(s)) break;
        h_(s.data, s.recv_ns);
        continue;
      }
      if (mode_ == WaitMode::Spin) {
        cpu_relax();
        continue;
      }
      if (!idle) {
        idle = true;
        idle_since = std::chrono::steady_clock::now();
      }
      if (std::chrono::steady_clock::now() - idle_since < std::chrono::microseconds(50)) {
        cpu_relax();
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    }
  }
  MutexQueue<FrameSlot> q_;
  WaitMode mode_;
  Handler h_;
  std::thread t_;
  std::atomic<bool> running_{false};
};

// Waits until steady_ns() >= target: sleep for long waits, spin the last bit
// (sleep alone is too coarse for sub-millisecond pacing).
void wait_until(std::int64_t target) {
  std::int64_t now = steady_ns();
  if (target - now > 300'000)
    std::this_thread::sleep_for(std::chrono::nanoseconds(target - now - 200'000));
  while (steady_ns() < target) cpu_relax();
}

struct Result {
  LatencyHistogram internal, service;
  std::uint64_t frames = 0, published_frames = 0, lines = 0;
  double wall_s = 0;
  std::string summary;
  std::uint64_t parse_errors = 0;
};

template <class Book, class ParserT, class Pipeline>
Result run_latency(const std::vector<Frame>& frames, const Args& a) {
  Result res;
  FrameProcessor<Book, ParserT> proc({.stats_interval_s = 0, .log_resyncs = false});
  NullSink null_sink;
  StdoutSink stdout_sink;
  Sink& sink = a.stdout_sink ? static_cast<Sink&>(stdout_sink) : null_sink;
  Publisher pub(sink, 10);
  Pipeline pipe(4096, a.wait, [&](std::string_view raw, std::int64_t recv_ns) {
    const std::int64_t start = steady_ns();
    proc.on_frame(raw, recv_ns);
    pub.on_events(proc.events(), recv_ns);
    const std::int64_t done = steady_ns();
    if (!proc.events().empty()) {
      res.internal.record(done - recv_ns);
      res.service.record(done - start);
      ++res.published_frames;
    }
  });
  pipe.start();

  // Pacing: replay the recorded inter-frame gaps divided by speed (gaps
  // clamped to [0, 1 s] like the replayer).
  const std::int64_t t0 = steady_ns();
  double feed_ns = 0;
  for (int loop = 0; loop < a.loops; ++loop) {
    for (std::size_t i = 0; i < frames.size(); ++i) {
      if (i > 0) {
        const std::int64_t gap =
            std::clamp<std::int64_t>(frames[i].ts - frames[i - 1].ts, 0, 1'000'000'000);
        feed_ns += static_cast<double>(gap) / a.speed;
      }
      wait_until(t0 + static_cast<std::int64_t>(feed_ns));
      pipe.push(frames[i].raw, steady_ns());  // "received" now
      ++res.frames;
    }
  }
  pipe.stop();
  res.wall_s = static_cast<double>(steady_ns() - t0) / 1e9;
  res.lines = pub.lines_published();
  res.summary = proc.summary();
  res.parse_errors = proc.parse_errors();
  return res;
}

template <class Book, class ParserT>
Result run_throughput(const std::vector<Frame>& frames, const Args& a) {
  Result res;
  FrameProcessor<Book, ParserT> proc({.stats_interval_s = 0, .log_resyncs = false});
  NullSink sink;
  Publisher pub(sink, 10);
  const std::int64_t t0 = steady_ns();
  for (int loop = 0; loop < a.loops; ++loop) {
    for (const auto& f : frames) {
      proc.on_frame(f.raw, f.ts);
      pub.on_events(proc.events(), f.ts);
      ++res.frames;
    }
  }
  res.wall_s = static_cast<double>(steady_ns() - t0) / 1e9;
  res.lines = pub.lines_published();
  res.summary = proc.summary();
  res.parse_errors = proc.parse_errors();
  return res;
}

std::string read_file(const std::string& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto val = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (k == "--variant") {
      a.variant = val();
    } else if (k == "--wait") {
      a.wait = val() == "hybrid" ? WaitMode::Hybrid : WaitMode::Spin;
    } else if (k == "--speed") {
      a.speed = std::atof(val().c_str());
    } else if (k == "--loops") {
      a.loops = std::atoi(val().c_str());
    } else if (k == "--sink") {
      a.stdout_sink = val() == "stdout";
    } else if (k == "--throughput") {
      a.throughput = true;
    } else if (k == "--expect") {
      a.expect = val();
    } else if (k == "--out") {
      a.out = val();
    } else {
      a.files.push_back(k);
    }
  }
  if (a.files.empty() || (a.variant != "optimized" && a.variant != "naive") || a.speed <= 0) {
    std::fprintf(
        stderr,
        "usage: pm_latency_bench --variant optimized|naive [--wait spin|hybrid] [--speed X] "
        "[--loops N] [--throughput] [--expect F] [--out F] FILES...\n");
    return 2;
  }
  const auto frames = load(a.files);
  const bool naive = a.variant == "naive";
  std::fprintf(
      stderr, "loaded %zu frames; %s %s, %d loops%s\n", frames.size(), a.variant.c_str(),
      a.throughput ? "throughput" : "latency", a.loops,
      a.throughput ? "" : (" at " + std::to_string(static_cast<int>(a.speed)) + "x").c_str());

  Result r;
  if (a.throughput) {
    r = naive ? run_throughput<MapBook, NaiveParser>(frames, a)
              : run_throughput<FlatBook, Parser>(frames, a);
  } else {
    r = naive ? run_latency<MapBook, NaiveParser, MutexPipeline>(frames, a)
              : run_latency<FlatBook, Parser, FramePipeline>(frames, a);
  }

  // Correctness gate: the final books must equal the live run's.
  bool correct = r.parse_errors == 0;
  if (!a.expect.empty()) {
    const auto diffs = compare_summaries(read_file(a.expect), r.summary);
    correct = correct && diffs.empty();
    for (const auto& d : diffs) std::fprintf(stderr, "DIFF %s\n", d.c_str());
  }

  const double fps = static_cast<double>(r.frames) / r.wall_s;
  std::string json =
      "{\"variant\":\"" + a.variant + "\",\"stack\":\"" +
      (naive ? "nlohmann::json + std::map book + std::mutex queue"
             : "simdjson + flat-array book + lock-free SPSC queue") +
      "\",\"mode\":\"" + (a.throughput ? "throughput" : "latency") + "\",\"wait\":\"" +
      (a.wait == WaitMode::Spin ? "spin" : "hybrid") + "\",\"speed\":" + std::to_string(a.speed) +
      ",\"loops\":" + std::to_string(a.loops) + ",\"frames\":" + std::to_string(r.frames) +
      ",\"frames_with_output\":" + std::to_string(r.published_frames) +
      ",\"lines_published\":" + std::to_string(r.lines) +
      ",\"wall_s\":" + std::to_string(r.wall_s) + ",\"frames_per_s\":" + std::to_string(fps) +
      ",\"books_match_live_run\":" + (a.expect.empty() ? "null" : (correct ? "true" : "false")) +
      ",\"machine\":\"" + machine_description() + "\",\"build\":\"" + build_description() +
      "\",\"sink\":\"" +
      (a.stdout_sink ? std::string("stdout (write syscall per frame)")
                     : std::string("NullSink (lines fully formatted, bytes discarded)")) +
      "\"";
  if (!a.throughput) {
    json += ",\"internal_latency\":" + r.internal.to_json() +
            ",\"service_time\":" + r.service.to_json();
  }
  json += "}\n";
  if (!a.out.empty()) std::ofstream(a.out) << json;

  if (a.throughput) {
    std::fprintf(stderr, "[%s] %.0f frames/s (%.2f us/frame), %llu lines, correct=%s\n",
                 a.variant.c_str(), fps, 1e6 / fps, static_cast<unsigned long long>(r.lines),
                 correct ? "yes" : "NO");
  } else {
    auto us = [](std::uint64_t ns) { return static_cast<double>(ns) / 1e3; };
    std::fprintf(stderr,
                 "[%s/%s] internal p50 %.2f p90 %.2f p99 %.2f p99.9 %.2f max %.1f us | service p50 "
                 "%.2f p99 %.2f "
                 "us | n=%llu correct=%s\n",
                 a.variant.c_str(), a.wait == WaitMode::Spin ? "spin" : "hybrid",
                 us(r.internal.percentile(0.5)), us(r.internal.percentile(0.9)),
                 us(r.internal.percentile(0.99)), us(r.internal.percentile(0.999)),
                 us(r.internal.max()), us(r.service.percentile(0.5)),
                 us(r.service.percentile(0.99)),
                 static_cast<unsigned long long>(r.internal.count()), correct ? "yes" : "NO");
  }
  return correct ? 0 : 1;
}
