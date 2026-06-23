#include "oklib/base/thread_pool.h"

#include <utility>

namespace oklib {

ThreadPool::ThreadPool(std::string name) : name_(std::move(name)) {}

ThreadPool::~ThreadPool() {
  stop();
}

void ThreadPool::start(int thread_count) {
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

void ThreadPool::stop() {
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

void ThreadPool::run(Task task) {
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

std::size_t ThreadPool::queue_size() const {
  std::lock_guard lock(mutex_);
  return tasks_.size();
}

void ThreadPool::run_worker() {
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

}  // namespace oklib
