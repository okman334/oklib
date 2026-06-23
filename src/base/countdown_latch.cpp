#include "oklib/base/countdown_latch.h"

namespace oklib {

CountDownLatch::CountDownLatch(int count) : count_(count) {}

void CountDownLatch::wait() {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this] { return count_ == 0; });
}

void CountDownLatch::count_down() {
  std::lock_guard lock(mutex_);
  if (count_ > 0) {
    --count_;
    if (count_ == 0) {
      cv_.notify_all();
    }
  }
}

int CountDownLatch::count() const {
  std::lock_guard lock(mutex_);
  return count_;
}

}  // namespace oklib
