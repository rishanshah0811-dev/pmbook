#pragma once
// The "first version anyone would write" parser, for the before/after
// benchmark: nlohmann/json builds a full tree of the message (allocating a
// node for every value), then we read fields out of it.
//
// It fills the same ParsedFrame structs and uses the same exact fixed-point
// conversions as the fast Parser, so both produce identical books. The only
// difference being measured is the JSON approach.

#include <nlohmann/json.hpp>
#include <string_view>

#include "parse/messages.hpp"
#include "parse/parser.hpp"  // ParseStatus

namespace pm {

class NaiveParser {
 public:
  ParseStatus parse(std::string_view frame, ParsedFrame& out);
  const char* last_error() const noexcept { return error_; }

 private:
  bool parse_message(const nlohmann::json& o, ParsedFrame& out);
  bool fail(const char* why) noexcept {
    error_ = why;
    return false;
  }

  nlohmann::json doc_;  // owns the strings that `out`'s string_views point into
  const char* error_ = "";
};

}  // namespace pm
