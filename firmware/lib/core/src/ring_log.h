#pragma once
// Fixed-size ring buffer, used for the gateway's store-and-forward buffer
// (meter samples captured while WiFi or MQTT is down) and for the on-device
// event log shown in the web UI. Header-only and dependency-free so the native
// test environment can exercise it.

#include <cstddef>
#include <cstdint>

namespace core {

template <typename T, size_t N>
class RingLog {
 public:
  void push(const T& value) {
    buf_[cursor_] = value;
    cursor_ = (cursor_ + 1) % N;
    if (count_ < N) {
      ++count_;
    } else {
      ++dropped_;
    }
  }

  size_t size() const { return count_; }
  static constexpr size_t capacity() { return N; }
  bool empty() const { return count_ == 0; }
  bool full() const { return count_ == N; }
  uint32_t dropped() const { return dropped_; }

  // 0 = oldest, size()-1 = newest.
  bool at(size_t index, T& out) const {
    if (index >= count_) return false;
    const size_t oldest = (cursor_ + N - count_) % N;
    out = buf_[(oldest + index) % N];
    return true;
  }

  const T& newest() const {
    const size_t index = (cursor_ + N - 1) % N;
    return buf_[index];
  }

  // Copies up to `max` oldest entries into out and removes them, so a flush can
  // be batched without allocating.
  size_t popBatch(T* out, size_t max) {
    const size_t take = count_ < max ? count_ : max;
    const size_t oldest = (cursor_ + N - count_) % N;
    for (size_t i = 0; i < take; ++i) out[i] = buf_[(oldest + i) % N];
    count_ -= take;
    return take;
  }

  void clear() {
    count_ = 0;
    cursor_ = 0;
    dropped_ = 0;
  }

 private:
  T buf_[N] = {};
  size_t cursor_ = 0;
  size_t count_ = 0;
  uint32_t dropped_ = 0;
};

}  // namespace core
