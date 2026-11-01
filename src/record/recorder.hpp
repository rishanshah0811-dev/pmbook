#pragma once
// Writes every raw frame to disk so any session can be replayed later.
//
// Format (PMBook protocol v1, see CONTRACT.md): gzip-compressed JSON Lines,
// one record per line:  {"ts_recv_ns":<int>,"raw":"<original frame>"}
// Files rotate every UTC hour: pm_YYYYMMDD_HH.jsonl.gz
//
// Crash safety: gzip normally only finishes a file on close. We do a
// "sync flush" about once a second, so if the process dies, everything up
// to the last flush is still readable.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace pm {

class Recorder {
 public:
  // `now_wall_s` returns Unix time in seconds; injectable so tests can fake
  // the hour changing.
  explicit Recorder(std::string dir, std::function<std::int64_t()> now_wall_s = nullptr);
  ~Recorder();
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  // Appends one record. Returns false on I/O error.
  bool write(std::int64_t ts_recv_ns, std::string_view raw);
  void flush();
  void close();

  const std::string& current_path() const noexcept { return path_; }
  std::uint64_t records() const noexcept { return records_; }
  std::uint64_t bytes_in() const noexcept { return bytes_in_; }

  // "pm_20261101_19.jsonl.gz" for the UTC hour containing unix_s.
  static std::string file_name_for(std::int64_t unix_s);
  // Appends `s` to `out` as a JSON string body (no surrounding quotes),
  // escaping ", \ and control characters. UTF-8 passes through unchanged.
  static void append_json_escaped(std::string& out, std::string_view s);

 private:
  bool open_for(std::int64_t unix_s);

  std::string dir_;
  std::function<std::int64_t()> now_wall_s_;
  void* gz_ = nullptr;  // gzFile (kept opaque so zlib.h stays out of our header)
  std::string path_;
  std::int64_t hour_ = -1;
  std::int64_t last_flush_s_ = 0;
  std::string line_;  // reused buffer for building each line
  std::uint64_t records_ = 0;
  std::uint64_t bytes_in_ = 0;
};

}  // namespace pm
