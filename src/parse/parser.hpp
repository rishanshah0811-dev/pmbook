#pragma once
// Fast parser for market-channel frames, built on simdjson's On-Demand API.
// On-Demand doesn't build a tree of the whole document. It walks the JSON
// once, and we pull out values as we pass them.

#include <simdjson.h>

#include <cstddef>
#include <memory>
#include <string_view>

#include "parse/messages.hpp"

namespace pm {

enum class ParseStatus : std::uint8_t {
  Ok,     // `out` holds zero or more messages
  Pong,   // heartbeat reply, no messages
  Error,  // malformed frame; see Parser::last_error()
};

class Parser {
 public:
  Parser() = default;
  // Holds big internal buffers; copying one would be a mistake, so forbid it.
  Parser(const Parser&) = delete;
  Parser& operator=(const Parser&) = delete;

  // Parses one raw WebSocket text frame into `out` (cleared first).
  // string_views in `out` stay valid until the next call.
  ParseStatus parse(std::string_view frame, ParsedFrame& out);

  // Human-readable reason for the last Error. Static string, never freed.
  const char* last_error() const noexcept { return error_; }

 private:
  bool parse_message(simdjson::ondemand::object obj, ParsedFrame& out);
  bool parse_levels(simdjson::ondemand::value v, ParsedFrame& out, Range& range);
  bool parse_changes(simdjson::ondemand::value v, ParsedFrame& out, Range& range);
  bool fail(const char* why) noexcept {
    error_ = why;
    return false;
  }

  simdjson::ondemand::parser parser_;
  // simdjson reads up to SIMDJSON_PADDING bytes past the end of the input
  // (it loads 32/64 bytes at a time), so we copy each frame into a buffer
  // that has that much spare room. unique_ptr<char[]> frees it automatically
  // when the Parser is destroyed. That's RAII: the owner's lifetime is the
  // resource's lifetime.
  std::unique_ptr<char[]> buf_;
  std::size_t cap_ = 0;
  const char* error_ = "";
};

}  // namespace pm
