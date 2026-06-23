#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "oklib/base/noncopyable.h"

namespace oklib {

template <typename T>
class BlockingQueue : private Noncopyable {
 public:
  BlockingQueue() = default;

  void put(const T& value) {
    {
      std::lock_guard lock(mutex_);
      queue_.push_back(value);
    }
    cv_.notify_one();
  }

  void put(T&& value) {
    {
      std::lock_guard lock(mutex_);
      queue_.push_back(std::move(value));
    }
    cv_.notify_one();
  }

  T take() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return T{};
    }
    T value = std::move(queue_.front());
    queue_.pop_front();
    return value;
  }

  template <typename Rep, typename Period>
  std::optional<T> try_take_for(const std::chrono::duration<Rep, Period>& timeout) {
    std::unique_lock lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this] { return closed_ || !queue_.empty(); })) {
      return std::nullopt;
    }
    if (queue_.empty()) {
      return std::nullopt;
    }
    T value = std::move(queue_.front());
    queue_.pop_front();
    return value;
  }

  void close() {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  [[nodiscard]] bool empty() const {
    std::lock_guard lock(mutex_);
    return queue_.empty();
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<T> queue_;
  bool closed_{false};
};

}  // namespace oklib
