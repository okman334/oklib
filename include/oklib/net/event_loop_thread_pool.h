#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "oklib/base/noncopyable.h"

namespace oklib::net {

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool : private oklib::Noncopyable {
 public:
  using ThreadInitCallback = std::function<void(EventLoop*)>;

  EventLoopThreadPool(EventLoop* base_loop, std::string name);
  ~EventLoopThreadPool();

  void set_thread_num(int num_threads) noexcept { num_threads_ = num_threads; }
  void start(const ThreadInitCallback& callback = {});
  [[nodiscard]] EventLoop* get_next_loop();
  [[nodiscard]] std::vector<EventLoop*> all_loops() const;
  [[nodiscard]] bool started() const noexcept { return started_; }

 private:
  EventLoop* base_loop_;
  std::string name_;
  bool started_{false};
  int num_threads_{0};
  std::size_t next_{0};
  std::vector<std::unique_ptr<EventLoopThread>> threads_;
  std::vector<EventLoop*> loops_;
};

}  // namespace oklib::net
