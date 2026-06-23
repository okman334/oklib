#include "oklib/net/event_loop.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

#include "oklib/base/logging.h"
#include "oklib/net/channel.h"
#include "oklib/net/poller.h"

namespace oklib::net {
namespace {

thread_local EventLoop* loop_in_this_thread = nullptr;
constexpr int k_default_poll_timeout_ms = 10000;
std::once_flag ignore_sigpipe_once;

void set_non_blocking_and_close_on_exec(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  const int fd_flags = ::fcntl(fd, F_GETFD, 0);
  if (fd_flags >= 0) {
    ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  }
}

}  // namespace

EventLoop* EventLoop::current() {
  return loop_in_this_thread;
}

EventLoop::EventLoop() : thread_id_(std::this_thread::get_id()), poller_(std::make_unique<Poller>(this)) {
  std::call_once(ignore_sigpipe_once, [] { ::signal(SIGPIPE, SIG_IGN); });
  if (loop_in_this_thread != nullptr) {
    throw std::logic_error("only one EventLoop may exist per thread");
  }
  loop_in_this_thread = this;

  if (::pipe(wakeup_fds_) != 0) {
    throw std::system_error(errno, std::generic_category(), "pipe");
  }
  set_non_blocking_and_close_on_exec(wakeup_fds_[0]);
  set_non_blocking_and_close_on_exec(wakeup_fds_[1]);

  wakeup_channel_ = std::make_unique<Channel>(this, wakeup_fds_[0]);
  wakeup_channel_->set_read_callback([this](oklib::Timestamp time) { handle_wakeup(time); });
  wakeup_channel_->enable_reading();
}

EventLoop::~EventLoop() {
  if (wakeup_channel_) {
    wakeup_channel_->disable_all();
    wakeup_channel_->remove();
  }
  if (wakeup_fds_[0] >= 0) {
    ::close(wakeup_fds_[0]);
  }
  if (wakeup_fds_[1] >= 0) {
    ::close(wakeup_fds_[1]);
  }
  loop_in_this_thread = nullptr;
}

void EventLoop::loop() {
  assert_in_loop_thread();
  looping_ = true;
  quit_ = false;

  while (!quit_.load(std::memory_order_acquire)) {
    active_channels_.clear();
    poll_return_time_ = poller_->poll(poll_timeout_ms(), &active_channels_);
    for (Channel* channel : active_channels_) {
      channel->handle_event(poll_return_time_);
    }
    run_due_timers();
    do_pending_functors();
  }

  looping_ = false;
}

void EventLoop::quit() {
  quit_ = true;
  if (!is_in_loop_thread()) {
    wakeup();
  }
}

void EventLoop::run_in_loop(Functor callback) {
  if (is_in_loop_thread()) {
    callback();
  } else {
    queue_in_loop(std::move(callback));
  }
}

void EventLoop::queue_in_loop(Functor callback) {
  {
    std::lock_guard lock(pending_mutex_);
    pending_functors_.push_back(std::move(callback));
  }
  if (!is_in_loop_thread() || calling_pending_functors_) {
    wakeup();
  }
}

std::size_t EventLoop::queue_size() const {
  std::lock_guard lock(pending_mutex_);
  return pending_functors_.size();
}

void EventLoop::cancel(TimerId timer_id) {
  if (!timer_id.valid()) {
    return;
  }
  {
    std::lock_guard lock(timer_mutex_);
    canceled_timers_.insert(timer_id.value());
  }
  wakeup();
}

void EventLoop::update_channel(Channel* channel) {
  assert_in_loop_thread();
  poller_->update_channel(channel);
}

void EventLoop::remove_channel(Channel* channel) {
  assert_in_loop_thread();
  poller_->remove_channel(channel);
}

bool EventLoop::has_channel(Channel* channel) const {
  return poller_->has_channel(channel);
}

void EventLoop::assert_in_loop_thread() const {
  if (!is_in_loop_thread()) {
    abort_not_in_loop_thread();
  }
}

void EventLoop::wakeup() {
  uint64_t one = 1;
  const char* data = reinterpret_cast<const char*>(&one);
  const auto n = ::write(wakeup_fds_[1], data, sizeof(one));
  if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    OKLIB_LOG_ERROR << "EventLoop wakeup write failed: " << std::strerror(errno);
  }
}

TimerId EventLoop::add_timer(Clock::time_point when, Clock::duration interval, TimerCallback callback) {
  TimerId id;
  {
    std::lock_guard lock(timer_mutex_);
    id = TimerId(next_timer_id_++);
    timers_.push_back(Timer{id, when, interval, std::move(callback)});
  }
  wakeup();
  return id;
}

int EventLoop::poll_timeout_ms() const {
  std::lock_guard lock(timer_mutex_);
  if (timers_.empty()) {
    return k_default_poll_timeout_ms;
  }

  const auto now = Clock::now();
  auto next = timers_.front().when;
  for (const auto& timer : timers_) {
    if (timer.when < next) {
      next = timer.when;
    }
  }
  if (next <= now) {
    return 0;
  }
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
  return static_cast<int>(std::min<int64_t>(millis, k_default_poll_timeout_ms));
}

void EventLoop::run_due_timers() {
  std::vector<Timer> expired;
  const auto now = Clock::now();

  {
    std::lock_guard lock(timer_mutex_);
    std::vector<Timer> pending;
    pending.reserve(timers_.size());
    for (auto& timer : timers_) {
      if (canceled_timers_.contains(timer.id.value())) {
        canceled_timers_.erase(timer.id.value());
      } else if (timer.when <= now) {
        expired.push_back(std::move(timer));
      } else {
        pending.push_back(std::move(timer));
      }
    }
    timers_.swap(pending);
  }

  for (auto& timer : expired) {
    timer.callback();
    if (timer.interval != Clock::duration::zero()) {
      std::lock_guard lock(timer_mutex_);
      if (!canceled_timers_.contains(timer.id.value())) {
        timer.when = Clock::now() + timer.interval;
        timers_.push_back(std::move(timer));
      } else {
        canceled_timers_.erase(timer.id.value());
      }
    }
  }
}

void EventLoop::handle_wakeup(oklib::Timestamp) {
  uint64_t value = 0;
  for (;;) {
    const auto n = ::read(wakeup_fds_[0], &value, sizeof(value));
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    if (n <= 0 || n < static_cast<ssize_t>(sizeof(value))) {
      break;
    }
  }
}

void EventLoop::do_pending_functors() {
  std::vector<Functor> functors;
  calling_pending_functors_ = true;
  {
    std::lock_guard lock(pending_mutex_);
    functors.swap(pending_functors_);
  }
  for (auto& functor : functors) {
    functor();
  }
  calling_pending_functors_ = false;
}

void EventLoop::abort_not_in_loop_thread() const {
  throw std::logic_error("EventLoop operation called from a non-owner thread");
}

}  // namespace oklib::net
