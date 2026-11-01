#include "record/recorder.hpp"

#include <zlib.h>

#include <charconv>
#include <ctime>
#include <filesystem>
#include <utility>

#include "core/clock.hpp"
#include "core/json.hpp"

namespace pm {

namespace {
gzFile as_gz(void* p) { return static_cast<gzFile>(p); }
}  // namespace

Recorder::Recorder(std::string dir, std::function<std::int64_t()> now_wall_s)
    : dir_(std::move(dir)), now_wall_s_(now_wall_s ? std::move(now_wall_s) : wall_seconds) {
  std::filesystem::create_directories(dir_);
}

Recorder::~Recorder() { close(); }

std::string Recorder::file_name_for(std::int64_t unix_s) {
  const std::time_t t = static_cast<std::time_t>(unix_s);
  std::tm tm{};
  gmtime_r(&t, &tm);  // UTC, so names don't depend on the machine's timezone
  char buf[32];
  std::strftime(buf, sizeof buf, "pm_%Y%m%d_%H.jsonl.gz", &tm);
  return buf;
}

void Recorder::append_json_escaped(std::string& out, std::string_view s) {
  pm::append_json_escaped(out, s);
}

bool Recorder::open_for(std::int64_t unix_s) {
  close();
  hour_ = unix_s / 3600;
  path_ = (std::filesystem::path(dir_) / file_name_for(unix_s)).string();
  // "ab": append. Restarting within the same hour adds a second gzip member
  // to the same file; gzip readers handle concatenated members.
  gz_ = gzopen(path_.c_str(), "ab6");
  return gz_ != nullptr;
}

bool Recorder::write(std::int64_t ts_recv_ns, std::string_view raw) {
  const std::int64_t now = now_wall_s_();
  if (now / 3600 != hour_ || gz_ == nullptr) {
    if (!open_for(now)) return false;
  }

  line_.clear();
  line_ += R"({"ts_recv_ns":)";
  char num[24];
  auto [end, ec] = std::to_chars(num, num + sizeof num, ts_recv_ns);
  line_.append(num, end);
  line_ += R"(,"raw":")";
  append_json_escaped(line_, raw);
  line_ += "\"}\n";

  const auto len = static_cast<unsigned>(line_.size());
  if (gzwrite(as_gz(gz_), line_.data(), len) != static_cast<int>(len)) return false;
  ++records_;
  bytes_in_ += raw.size();

  if (now != last_flush_s_) {
    gzflush(as_gz(gz_), Z_SYNC_FLUSH);
    last_flush_s_ = now;
  }
  return true;
}

void Recorder::flush() {
  if (gz_) gzflush(as_gz(gz_), Z_SYNC_FLUSH);
}

void Recorder::close() {
  if (gz_) {
    gzclose(as_gz(gz_));
    gz_ = nullptr;
  }
}

}  // namespace pm
