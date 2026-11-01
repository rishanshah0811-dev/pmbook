#pragma once
// Reads pm_*.jsonl.gz files written by Recorder, one record at a time.
// Streams through gzip in 64 KB chunks, so even huge files use little memory.
//
// Tolerates a file whose writer crashed (no gzip end marker, or a half-written
// last line): every complete line is returned, and truncated() reports it.

#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace pm {

struct Record {
  std::int64_t ts_recv_ns = 0;
  std::string_view raw;  // valid until the next call to next()
};

class RecordReader {
 public:
  RecordReader() = default;
  ~RecordReader();
  RecordReader(const RecordReader&) = delete;
  RecordReader& operator=(const RecordReader&) = delete;

  bool open(const std::string& path);
  // Returns false at end of file or on a malformed line (see error()).
  bool next(Record& out);
  void close();

  bool truncated() const noexcept { return truncated_; }
  const std::string& error() const noexcept { return error_; }
  std::uint64_t lines() const noexcept { return lines_; }

 private:
  bool fill();  // reads the next compressed chunk into buf_

  void* gz_ = nullptr;  // gzFile
  std::string buf_;     // decompressed bytes not yet consumed
  std::size_t pos_ = 0;
  bool eof_ = false;
  bool truncated_ = false;
  std::string error_;
  std::uint64_t lines_ = 0;

  simdjson::ondemand::parser parser_;
  std::unique_ptr<char[]> line_buf_;  // padded copy of the current line
  std::size_t line_cap_ = 0;
};

}  // namespace pm
