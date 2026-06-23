#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "oklib/base/noncopyable.h"
#include "oklib/base/timestamp.h"
#include "oklib/net/timer_id.h"

namespace oklib::net {

class Channel;
class Poller;

class EventLoop : private oklib::Noncopyable {
 public:
  using Functor = std::function<void()>;
  using TimerCallback = std::function<void()>;
  using Clock = std::chrono::steady_clock;

  EventLoop();
  ~EventLoop();

  void loop();
  void quit();

  void run_in_loop(Functor callback);
  void queue_in_loop(Functor callback);
  [[nodiscard]] std::size_t queue_size() const;

  template <typename Rep, typename Period>
  TimerId run_after(std::chrono::duration<Rep, Period> delay, TimerCallback callback) {
    return add_timer(Clock::now() + std::chrono::duration_cast<Clock::duration>(delay),
                     Clock::duration::zero(), std::move(callback));
  }

  template <typename Rep, typename Period>
  TimerId run_every(std::chrono::duration<Rep, Period> interval, TimerCallback callback) {
    const auto cast_interval = std::chrono::duration_cast<Clock::duration>(interval);
    return add_timer(Clock::now() + cast_interval, cast_interval, std::move(callback));
  }

  void cancel(TimerId timer_id);

  void update_channel(Channel* channel);
  void remove_channel(Channel* channel);
  [[nodiscard]] bool has_channel(Channel* channel) const;

  [[nodiscard]] bool is_in_loop_thread() const noexcept {
    return thread_id_ == std::this_thread::get_id();
  }
  void assert_in_loop_thread() const;

  [[nodiscard]] oklib::Timestamp poll_return_time() const noexcept { return poll_return_time_; }

  void wakeup();
  static EventLoop* current();

 private:
  struct Timer {
    TimerId id;
    Clock::time_point when;
    Clock::duration interval;
    TimerCallback callback;
  };

  TimerId add_timer(Clock::time_point when, Clock::duration interval, TimerCallback callback);
  int poll_timeout_ms() const;
  void run_due_timers();
  void handle_wakeup(oklib::Timestamp);
  void do_pending_functors();
  void abort_not_in_loop_thread() const;

  std::atomic<bool> looping_{false};
  std::atomic<bool> quit_{false};
  std::atomic<bool> calling_pending_functors_{false};
  const std::thread::id thread_id_;

  std::unique_ptr<Poller> poller_;
  oklib::Timestamp poll_return_time_;
  std::vector<Channel*> active_channels_;

  int wakeup_fds_[2]{-1, -1};
  std::unique_ptr<Channel> wakeup_channel_;

  mutable std::mutex pending_mutex_;
  std::vector<Functor> pending_functors_;

  mutable std::mutex timer_mutex_;
  std::vector<Timer> timers_;
  std::unordered_set<uint64_t> canceled_timers_;
  uint64_t next_timer_id_{1};
};

}  // namespace oklib::net
