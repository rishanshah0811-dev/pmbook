// FlatBook vs MapBook on an identical, pre-generated stream of updates.
// The stream mimics a live book: ~30 levels per side, most updates within a
// few cents of the top, ~35% of updates remove a level.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <vector>

#include "book/book_math.hpp"
#include "book/flat_book.hpp"
#include "book/map_book.hpp"

namespace {

using namespace pm;

struct Op {
  Side side;
  Price price;
  Size size;
};

struct Workload {
  std::vector<Level> bids, asks;
  std::vector<Op> ops;
};

const Workload& workload() {
  static const Workload w = [] {
    Workload out;
    std::mt19937_64 rng(7);
    std::uniform_int_distribution<Size> sz(1'000'000, 500'000'000'000);
    for (int k = 1; k <= 30; ++k) {
      out.bids.push_back({5000 - k * 10, sz(rng)});
      out.asks.push_back({5000 + k * 10, sz(rng)});
    }
    int mid = 5000;
    std::uniform_int_distribution<int> pct(0, 99);
    for (int i = 0; i < 1 << 16; ++i) {
      mid = std::clamp(mid + std::uniform_int_distribution<int>(-1, 1)(rng), 500, 9500);
      const Side side = pct(rng) < 50 ? Side::Buy : Side::Sell;
      const int off = std::uniform_int_distribution<int>(1, 300)(rng) / 10 * 10;  // 0.001 grid
      const Price p = side == Side::Buy ? mid - off : mid + off;
      out.ops.push_back({side, p, pct(rng) < 35 ? 0 : sz(rng)});
    }
    return out;
  }();
  return w;
}

// Apply one update, then read best bid/ask, which is what every consumer does.
template <class Book>
void BM_UpdateAndReadTop(benchmark::State& state) {
  const Workload& w = workload();
  auto book = std::make_unique<Book>();
  book->apply_snapshot(w.bids, w.asks);
  std::size_t i = 0;
  for (auto _ : state) {
    const Op& op = w.ops[i++ & (w.ops.size() - 1)];
    book->set_level(op.side, op.price, op.size);
    benchmark::DoNotOptimize(book->best_bid());
    benchmark::DoNotOptimize(book->best_ask());
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

// Apply one update, then extract the top 10 levels per side, which is what
// the publisher does for every book message.
template <class Book>
void BM_UpdateAndTop10(benchmark::State& state) {
  const Workload& w = workload();
  auto book = std::make_unique<Book>();
  book->apply_snapshot(w.bids, w.asks);
  std::array<Level, 10> out{};
  std::size_t i = 0;
  for (auto _ : state) {
    const Op& op = w.ops[i++ & (w.ops.size() - 1)];
    book->set_level(op.side, op.price, op.size);
    benchmark::DoNotOptimize(book->top_levels(Side::Buy, out));
    benchmark::DoNotOptimize(book->top_levels(Side::Sell, out));
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  state.counters["levels"] =
      static_cast<double>(book->level_count(Side::Buy) + book->level_count(Side::Sell));
}

// Replace the whole book with a 30+30 level snapshot.
template <class Book>
void BM_Snapshot30x30(benchmark::State& state) {
  const Workload& w = workload();
  auto book = std::make_unique<Book>();
  for (auto _ : state) {
    book->apply_snapshot(w.bids, w.asks);
    benchmark::DoNotOptimize(book->best_bid());
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

}  // namespace

BENCHMARK(BM_UpdateAndReadTop<pm::FlatBook>);
BENCHMARK(BM_UpdateAndReadTop<pm::MapBook>);
BENCHMARK(BM_UpdateAndTop10<pm::FlatBook>);
BENCHMARK(BM_UpdateAndTop10<pm::MapBook>);
BENCHMARK(BM_Snapshot30x30<pm::FlatBook>);
BENCHMARK(BM_Snapshot30x30<pm::MapBook>);
