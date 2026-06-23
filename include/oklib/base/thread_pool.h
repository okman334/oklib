#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "oklib/base/noncopyable.h"

namespace oklib {

class ThreadPool : private Noncopyable {
 public:
  using Task = std::function<void()>;

  explicit ThreadPool(std::string name = "ThreadPool") : name_(std::move(name)) {}

  ~ThreadPool() { stop(); }

  void start(int thread_count) {
    std::lock_guard lock(mutex_);
    if (running_ || thread_count <= 0) {
      running_ = true;
      return;
    }
    running_ = true;
    workers_.reserve(static_cast<std::size_t>(thread_count));
    for (int i = 0; i < thread_count; ++i) {
      workers_.emplace_back([this] { run_worker(); });
    }
  }

  void stop() {
    {
      std::lock_guard lock(mutex_);
      if (!running_ && workers_.empty()) {
        return;
      }
      running_ = false;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

  void run(Task task) {
    {
      std::lock_guard lock(mutex_);
      if (!running_ && workers_.empty()) {
        task();
        return;
      }
      tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
  }

  [[nodiscard]] const std::string& name() const noexcept { return name_; }

  [[nodiscard]] std::size_t queue_size() const {
    std::lock_guard lock(mutex_);
    return tasks_.size();
  }

 private:
  void run_worker() {
    for (;;) {
      Task task;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return !running_ || !tasks_.empty(); });
        if (!running_ && tasks_.empty()) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }

  std::string name_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Task> tasks_;
  std::vector<std::thread> workers_;
  bool running_{false};
};

}  // namespace oklib
