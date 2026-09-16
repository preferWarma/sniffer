#pragma once

#include <chrono>
#include <cstdint>

namespace sniffer::internal {

class NanosecondTimer {
 public:
  explicit NanosecondTimer(uint64_t* target) : target_(target) {
    if (target_) {
      start_ = Clock::now();
    }
  }

  NanosecondTimer(const NanosecondTimer&) = delete;
  NanosecondTimer& operator=(const NanosecondTimer&) = delete;

  ~NanosecondTimer() {
    if (target_) {
      *target_ += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count());
    }
  }

 private:
  using Clock = std::chrono::steady_clock;

  uint64_t* target_ = nullptr;
  Clock::time_point start_{};
};

}  // namespace sniffer::internal
