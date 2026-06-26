#include "oklib/net/timer_thread.h"

#include <future>
#include <utility>

#include "oklib/net/event_loop.h"
#include "oklib/net/event_loop_thread.h"

namespace oklib::net {
namespace {

std::mutex global_timer_mutex;
std::unique_ptr<TimerThread> global_timer;

}  // namespace

TimerThread::TimerThread(std::string name)
    : thread_(std::make_unique<EventLoopThread>(EventLoopThread::ThreadInitCallback{}, std::move(name))) {
  loop_ = thread_->start_loop();
}

TimerThread::~TimerThread() {
  stop();
}

void TimerThread::clear(TimerId timer_id) {
  std::lock_guard lock(mutex_);
  EventLoop* loop = loop_;
  if (loop != nullptr) {
    loop->cancel(timer_id);
  }
}

void TimerThread::stop() {
  std::unique_ptr<EventLoopThread> thread;
  {
    std::lock_guard lock(mutex_);
    loop_ = nullptr;
    thread = std::move(thread_);
  }
}

TimerId TimerThread::schedule(std::chrono::milliseconds delay,
                              std::uint32_t repeat,
                              TimerCallback callback) {
  return schedule(delay, repeat, std::move(callback), {});
}

TimerId TimerThread::schedule(std::chrono::milliseconds delay,
                              std::uint32_t repeat,
                              TimerCallback callback,
                              TimerId timer_id) {
  std::lock_guard lock(mutex_);
  EventLoop* loop = loop_;
  if (loop == nullptr || !callback) {
    return {};
  }

  if (loop->is_in_loop_thread()) {
    return loop->set_timer(delay, std::move(callback), repeat, timer_id);
  }

  auto ready = std::make_shared<std::promise<TimerId>>();
  auto future = ready->get_future();
  loop->run_in_loop([loop, delay, repeat, timer_id, callback = std::move(callback),
                     ready]() mutable {
    ready->set_value(loop->set_timer(delay, std::move(callback), repeat, timer_id));
  });
  return future.get();
}

TimerThread& GlobalTimer::instance() {
  std::lock_guard lock(global_timer_mutex);
  if (!global_timer) {
    global_timer = std::make_unique<TimerThread>("oklib-global-timer");
  }
  return *global_timer;
}

void GlobalTimer::clear(TimerId timer_id) {
  std::lock_guard lock(global_timer_mutex);
  if (global_timer) {
    global_timer->clear(timer_id);
  }
}

void GlobalTimer::shutdown() {
  std::unique_ptr<TimerThread> timer;
  {
    std::lock_guard lock(global_timer_mutex);
    timer = std::move(global_timer);
  }
}

}  // namespace oklib::net
