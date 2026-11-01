// Milestone 1 smoke tests: prove the toolchain and every hot-path dependency
// compile, link and run. Real tests arrive with the parser in milestone 2.
#include <gtest/gtest.h>
#include <simdjson.h>

#include <nlohmann/json.hpp>

#include "core/version.hpp"

TEST(Smoke, VersionIsSet) { EXPECT_EQ(pm::version(), "0.1.0"); }

TEST(Smoke, SimdjsonParsesAPolymarketStyleFrame) {
  // Shape copied from the docs' last_trade_price example.
  auto json = R"({"event_type":"last_trade_price","price":"0.08","size":"219.217767"})"_padded;
  simdjson::ondemand::parser parser;
  auto doc = parser.iterate(json);
  std::string_view type = doc["event_type"].get_string().value();
  EXPECT_EQ(type, "last_trade_price");
}

TEST(Smoke, NlohmannParsesTheSameFrame) {
  auto j = nlohmann::json::parse(R"({"event_type":"book","bids":[],"asks":[]})");
  EXPECT_EQ(j["event_type"], "book");
  EXPECT_TRUE(j["bids"].empty());
}
