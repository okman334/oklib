#pragma once

#include <condition_variable>
#include <mutex>

#include "oklib/base/noncopyable.h"

namespace oklib {

class CountDownLatch : private Noncopyable {
 public:
  explicit CountDownLatch(int count) : count_(count) {}

  void wait() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return count_ == 0; });
  }

  void count_down() {
    std::lock_guard lock(mutex_);
    if (count_ > 0) {
      --count_;
      if (count_ == 0) {
        cv_.notify_all();
      }
    }
  }

  [[nodiscard]] int count() const {
    std::lock_guard lock(mutex_);
    return count_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  int count_;
};

}  // namespace oklib
