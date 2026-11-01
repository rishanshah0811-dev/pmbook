#pragma once
// The "normal" way to pass data between threads: a ring buffer protected by
// a std::mutex. Same interface as SpscQueue, used only as the benchmark
// baseline. Correct for any number of threads, but every push and pop takes
// the lock, and a thread that finds it held may be put to sleep by the OS.

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace pm {

template <class T>
class MutexQueue {
 public:
  explicit MutexQueue(std::size_t capacity) : slots_(capacity) {}

  template <class U>
  bool try_push(U&& value) {
    std::lock_guard lock(mu_);
    if (size_ == slots_.size()) return false;
    slots_[(head_ + size_) % slots_.size()] = std::forward<U>(value);
    ++size_;
    return true;
  }

  bool try_pop(T& out) {
    std::lock_guard lock(mu_);
    if (size_ == 0) return false;
    out = std::move(slots_[head_]);
    head_ = (head_ + 1) % slots_.size();
    --size_;
    return true;
  }

 private:
  std::mutex mu_;
  std::vector<T> slots_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

}  // namespace pm
