#pragma once
// Tiny JSON writing helpers shared by the recorder and the publisher.

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace pm {

// Appends `s` as the body of a JSON string (no surrounding quotes), escaping
// ", \ and control characters. UTF-8 passes through unchanged.
inline void append_json_escaped(std::string& out, std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out += kHex[c >> 4];
          out += kHex[c & 0xF];
        } else {
          out += ch;
        }
    }
  }
}

inline void append_int(std::string& out, std::int64_t v) {
  char buf[24];
  auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, end);
}

}  // namespace pm
