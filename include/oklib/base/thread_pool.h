#pragma once

#include <deque>
#include <functional>
#include <condition_variable>
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

  explicit ThreadPool(std::string name = "ThreadPool");

  ~ThreadPool();

  void start(int thread_count);

  void stop();

  void run(Task task);

  [[nodiscard]] const std::string& name() const noexcept { return name_; }

  [[nodiscard]] std::size_t queue_size() const;

 private:
  void run_worker();

  std::string name_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Task> tasks_;
  std::vector<std::thread> workers_;
  bool running_{false};
};

}  // namespace oklib
