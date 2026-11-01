// Placeholder benchmark so the benchmark library is wired up from day one.
#include <benchmark/benchmark.h>
#include <simdjson.h>

static void BM_SimdjsonSmallFrame(benchmark::State& state) {
  auto json = R"({"event_type":"last_trade_price","price":"0.08","size":"219.217767"})"_padded;
  simdjson::ondemand::parser parser;
  for (auto _ : state) {
    auto doc = parser.iterate(json);
    std::string_view type = doc["event_type"].get_string().value();
    benchmark::DoNotOptimize(type);
  }
}
BENCHMARK(BM_SimdjsonSmallFrame);
