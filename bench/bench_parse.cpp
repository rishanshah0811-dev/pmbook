// Parser micro-benchmarks on real captured frames: simdjson fast path vs a
// straightforward nlohmann/json parse of the same bytes.
#include <benchmark/benchmark.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "parse/parser.hpp"

namespace {

// Loads the raw frames from the live fixture (one JSONL record per line).
std::vector<std::string> load_frames(bool want_array) {
  std::ifstream in(std::string(PM_TEST_DATA_DIR) + "/live_frames.jsonl");
  std::vector<std::string> out;
  std::string line;
  while (std::getline(in, line)) {
    auto raw = nlohmann::json::parse(line).at("raw").get<std::string>();
    if (raw == "PONG") continue;
    if ((raw.front() == '[') == want_array) out.push_back(std::move(raw));
  }
  return out;
}

void run_simdjson(benchmark::State& state, const std::vector<std::string>& frames) {
  pm::Parser parser;
  pm::ParsedFrame frame;
  std::size_t bytes = 0;
  for (auto _ : state) {
    for (const auto& f : frames) {
      auto s = parser.parse(f, frame);
      benchmark::DoNotOptimize(s);
      benchmark::DoNotOptimize(frame.messages.data());
      bytes += f.size();
    }
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(frames.size()));
}

void run_nlohmann(benchmark::State& state, const std::vector<std::string>& frames) {
  std::size_t bytes = 0;
  for (auto _ : state) {
    for (const auto& f : frames) {
      auto j = nlohmann::json::parse(f);
      benchmark::DoNotOptimize(j);
      bytes += f.size();
    }
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(frames.size()));
}

void BM_Simdjson_PriceChange(benchmark::State& s) { run_simdjson(s, load_frames(false)); }
void BM_Simdjson_BookSnapshotArray(benchmark::State& s) { run_simdjson(s, load_frames(true)); }
// nlohmann only builds a DOM here (no fixed-point conversion), so this
// comparison flatters the naive side. The full naive pipeline comes in M8.
void BM_NlohmannDomOnly_PriceChange(benchmark::State& s) { run_nlohmann(s, load_frames(false)); }
void BM_NlohmannDomOnly_BookSnapshotArray(benchmark::State& s) {
  run_nlohmann(s, load_frames(true));
}

}  // namespace

BENCHMARK(BM_Simdjson_PriceChange);
BENCHMARK(BM_NlohmannDomOnly_PriceChange);
BENCHMARK(BM_Simdjson_BookSnapshotArray);
BENCHMARK(BM_NlohmannDomOnly_BookSnapshotArray);
