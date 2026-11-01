#include "record/record_reader.hpp"

#include <zlib.h>

#include <cstring>

namespace pm {

namespace od = simdjson::ondemand;

RecordReader::~RecordReader() { close(); }

bool RecordReader::open(const std::string& path) {
  close();
  buf_.clear();
  pos_ = 0;
  eof_ = truncated_ = false;
  error_.clear();
  lines_ = 0;
  gz_ = gzopen(path.c_str(), "rb");
  if (!gz_) {
    error_ = "cannot open " + path;
    return false;
  }
  gzbuffer(static_cast<gzFile>(gz_), 1 << 16);
  return true;
}

void RecordReader::close() {
  if (gz_) {
    gzclose(static_cast<gzFile>(gz_));
    gz_ = nullptr;
  }
}

bool RecordReader::fill() {
  if (eof_ || !gz_) return false;
  // Drop consumed bytes so the buffer doesn't grow forever.
  buf_.erase(0, pos_);
  pos_ = 0;
  constexpr std::size_t kChunk = 1 << 16;
  const std::size_t old = buf_.size();
  buf_.resize(old + kChunk);
  const int n = gzread(static_cast<gzFile>(gz_), buf_.data() + old, static_cast<unsigned>(kChunk));
  if (n <= 0) {
    buf_.resize(old);
    eof_ = true;
    int errnum = Z_OK;
    gzerror(static_cast<gzFile>(gz_), &errnum);
    // Z_BUF_ERROR at the end means the stream stopped mid-way (crashed writer).
    if (n < 0 || errnum == Z_BUF_ERROR) truncated_ = true;
    return false;
  }
  buf_.resize(old + static_cast<std::size_t>(n));
  return true;
}

bool RecordReader::next(Record& out) {
  // Find the next complete line.
  std::size_t nl;
  while ((nl = buf_.find('\n', pos_)) == std::string::npos) {
    if (!fill()) {
      if (pos_ < buf_.size()) truncated_ = true;  // half-written last line: drop it
      return false;
    }
  }
  const std::size_t len = nl - pos_;
  const std::size_t need = len + simdjson::SIMDJSON_PADDING;
  if (line_cap_ < need) {
    line_cap_ = need * 2;
    line_buf_ = std::make_unique<char[]>(line_cap_);
  }
  std::memcpy(line_buf_.get(), buf_.data() + pos_, len);
  std::memset(line_buf_.get() + len, 0, simdjson::SIMDJSON_PADDING);
  pos_ = nl + 1;
  ++lines_;

  od::document doc;
  od::object obj;
  std::int64_t ts = 0;
  std::string_view raw;
  if (parser_.iterate(line_buf_.get(), len, line_cap_).get(doc) || doc.get_object().get(obj) ||
      obj["ts_recv_ns"].get_int64().get(ts) || obj["raw"].get_string().get(raw)) {
    error_ = "malformed record on line " + std::to_string(lines_);
    return false;
  }
  out.ts_recv_ns = ts;
  out.raw = raw;
  return true;
}

}  // namespace pm
