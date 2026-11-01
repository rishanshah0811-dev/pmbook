#pragma once
// Where published lines go. The publisher formats; a Sink delivers.

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace pm {

class Sink {
 public:
  virtual ~Sink() = default;
  // Appends bytes (whole lines). Called only from the processing thread.
  virtual void write(std::string_view data) = 0;
  // Called once per input frame, after all its messages are written.
  virtual void flush() {}
  // TCP only: new clients waiting for their welcome snapshot.
  virtual bool has_new_clients() const noexcept { return false; }
  virtual void adopt_new_clients(const std::vector<std::string_view>& /*welcome_lines*/) {}
};

// Newline-delimited JSON on stdout. Logs go to stderr, so stdout stays clean.
class StdoutSink final : public Sink {
 public:
  void write(std::string_view data) override {
    if (std::fwrite(data.data(), 1, data.size(), stdout) != data.size() && !warned_) {
      std::fprintf(stderr, "warning: stdout write failed (consumer gone?); continuing\n");
      warned_ = true;
    }
  }
  void flush() override { std::fflush(stdout); }

 private:
  bool warned_ = false;
};

// Collects output in memory (tests, benchmarks).
class StringSink final : public Sink {
 public:
  void write(std::string_view data) override { out.append(data); }
  std::string out;
};

// Counts bytes but stores nothing (benchmarks that shouldn't measure I/O).
class NullSink final : public Sink {
 public:
  void write(std::string_view data) override { bytes += data.size(); }
  std::size_t bytes = 0;
};

}  // namespace pm
