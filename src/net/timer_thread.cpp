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
                              std::chrono::milliseconds interval,
                              TimerCallback callback) {
  std::lock_guard lock(mutex_);
  EventLoop* loop = loop_;
  if (loop == nullptr || !callback) {
    return {};
  }

  auto timer_id = std::make_shared<TimerId>();
  auto wrapped = [timer_id, callback = std::move(callback)] {
    callback(*timer_id);
  };

  if (loop->is_in_loop_thread()) {
    *timer_id = interval == std::chrono::milliseconds::zero()
                    ? loop->run_after(delay, std::move(wrapped))
                    : loop->run_every(interval, std::move(wrapped));
    return *timer_id;
  }

  auto ready = std::make_shared<std::promise<TimerId>>();
  auto future = ready->get_future();
  loop->run_in_loop([loop, timer_id, delay, interval, callback = std::move(wrapped),
                     ready]() mutable {
    *timer_id = interval == std::chrono::milliseconds::zero()
                    ? loop->run_after(delay, std::move(callback))
                    : loop->run_every(interval, std::move(callback));
    ready->set_value(*timer_id);
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
