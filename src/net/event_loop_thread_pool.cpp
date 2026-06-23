#include "oklib/net/event_loop_thread_pool.h"

#include <cassert>

#include "oklib/net/event_loop.h"
#include "oklib/net/event_loop_thread.h"

namespace oklib::net {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop, std::string name)
    : base_loop_(base_loop), name_(std::move(name)) {}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::start(const ThreadInitCallback& callback) {
  assert(!started_);
  base_loop_->assert_in_loop_thread();
  started_ = true;
  for (int i = 0; i < num_threads_; ++i) {
    auto thread = std::make_unique<EventLoopThread>(callback, name_ + "-" + std::to_string(i));
    loops_.push_back(thread->start_loop());
    threads_.push_back(std::move(thread));
  }
  if (num_threads_ == 0 && callback) {
    callback(base_loop_);
  }
}

EventLoop* EventLoopThreadPool::get_next_loop() {
  base_loop_->assert_in_loop_thread();
  EventLoop* loop = base_loop_;
  if (!loops_.empty()) {
    loop = loops_[next_];
    next_ = (next_ + 1) % loops_.size();
  }
  return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::all_loops() const {
  if (loops_.empty()) {
    return {base_loop_};
  }
  return loops_;
}

}  // namespace oklib::net
