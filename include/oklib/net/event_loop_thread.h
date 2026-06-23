#pragma once

#include <functional>
#include <future>
#include <string>
#include <thread>

#include "oklib/base/noncopyable.h"

namespace oklib::net {

class EventLoop;

class EventLoopThread : private oklib::Noncopyable {
 public:
  using ThreadInitCallback = std::function<void(EventLoop*)>;

  explicit EventLoopThread(ThreadInitCallback callback = {}, std::string name = {});
  ~EventLoopThread();

  EventLoop* start_loop();

 private:
  void thread_func(std::promise<EventLoop*> ready);

  ThreadInitCallback callback_;
  std::string name_;
  std::thread thread_;
  EventLoop* loop_{nullptr};
};

}  // namespace oklib::net
