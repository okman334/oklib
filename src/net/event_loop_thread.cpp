#include "oklib/net/event_loop_thread.h"

#include "oklib/net/event_loop.h"

namespace oklib::net {

EventLoopThread::EventLoopThread(ThreadInitCallback callback, std::string name)
    : callback_(std::move(callback)), name_(std::move(name)) {}

EventLoopThread::~EventLoopThread() {
  if (loop_ != nullptr) {
    loop_->quit();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

EventLoop* EventLoopThread::start_loop() {
  std::promise<EventLoop*> ready;
  auto future = ready.get_future();
  thread_ = std::thread([this, ready = std::move(ready)]() mutable { thread_func(std::move(ready)); });
  loop_ = future.get();
  return loop_;
}

void EventLoopThread::thread_func(std::promise<EventLoop*> ready) {
  EventLoop loop;
  if (callback_) {
    callback_(&loop);
  }
  ready.set_value(&loop);
  loop.loop();
  loop_ = nullptr;
}

}  // namespace oklib::net
