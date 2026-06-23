#pragma once

#include <condition_variable>
#include <mutex>

#include "oklib/base/noncopyable.h"

namespace oklib {

class CountDownLatch : private Noncopyable {
 public:
  explicit CountDownLatch(int count);

  void wait();

  void count_down();

  [[nodiscard]] int count() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  int count_;
};

}  // namespace oklib
