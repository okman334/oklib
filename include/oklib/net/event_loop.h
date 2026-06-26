#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
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
  using TimerIdCallback = std::function<void(TimerId)>;
  using Clock = std::chrono::steady_clock;
  static constexpr std::uint32_t k_infinite_repeats = std::numeric_limits<std::uint32_t>::max();

  EventLoop();
  ~EventLoop();

  void loop();
  void quit();

  void run_in_loop(Functor callback);
  void queue_in_loop(Functor callback);
  [[nodiscard]] std::size_t queue_size() const;

  template <typename Rep, typename Period>
  TimerId run_after(std::chrono::duration<Rep, Period> delay, TimerCallback callback) {
    auto wrapped = [callback = std::move(callback)](TimerId) {
      if (callback) {
        callback();
      }
    };
    return add_timer(Clock::now() + std::chrono::duration_cast<Clock::duration>(delay),
                     Clock::duration::zero(), 1, std::move(wrapped));
  }

  template <typename Rep, typename Period>
  TimerId run_every(std::chrono::duration<Rep, Period> interval, TimerCallback callback) {
    const auto cast_interval = std::chrono::duration_cast<Clock::duration>(interval);
    if (cast_interval <= Clock::duration::zero()) {
      return {};
    }
    auto wrapped = [callback = std::move(callback)](TimerId) {
      if (callback) {
        callback();
      }
    };
    return add_timer(Clock::now() + cast_interval, cast_interval, k_infinite_repeats, std::move(wrapped));
  }

  template <typename Rep, typename Period>
  TimerId set_timer(std::chrono::duration<Rep, Period> timeout,
                    TimerIdCallback callback,
                    std::uint32_t repeat = k_infinite_repeats,
                    TimerId timer_id = {}) {
    const auto cast_timeout = std::chrono::duration_cast<Clock::duration>(timeout);
    if (cast_timeout < Clock::duration::zero() || repeat == 0) {
      return {};
    }
    return add_timer(Clock::now() + cast_timeout, cast_timeout, repeat, std::move(callback), timer_id);
  }

  template <typename Rep, typename Period>
  TimerId set_timer(std::chrono::duration<Rep, Period> timeout,
                    TimerIdCallback callback,
                    std::uint32_t repeat,
                    std::uint64_t timer_id) {
    return set_timer(timeout, std::move(callback), repeat, TimerId(timer_id));
  }

  template <typename Rep, typename Period>
  TimerId set_timer_in_loop(std::chrono::duration<Rep, Period> timeout,
                            TimerIdCallback callback,
                            std::uint32_t repeat = k_infinite_repeats,
                            TimerId timer_id = {}) {
    return set_timer(timeout, std::move(callback), repeat, timer_id);
  }

  template <typename Rep, typename Period>
  TimerId set_timeout(std::chrono::duration<Rep, Period> timeout,
                      TimerIdCallback callback,
                      TimerId timer_id = {}) {
    return set_timer(timeout, std::move(callback), 1, timer_id);
  }

  template <typename Rep, typename Period>
  TimerId set_timeout(std::uint64_t timer_id,
                      std::chrono::duration<Rep, Period> timeout,
                      TimerIdCallback callback) {
    return set_timeout(timeout, std::move(callback), TimerId(timer_id));
  }

  template <typename Rep, typename Period>
  TimerId set_interval(std::chrono::duration<Rep, Period> interval,
                       TimerIdCallback callback,
                       TimerId timer_id = {}) {
    return set_timer(interval, std::move(callback), k_infinite_repeats, timer_id);
  }

  template <typename Rep, typename Period>
  TimerId set_interval(std::uint64_t timer_id,
                       std::chrono::duration<Rep, Period> interval,
                       TimerIdCallback callback) {
    return set_interval(interval, std::move(callback), TimerId(timer_id));
  }

  void cancel(TimerId timer_id);
  void kill_timer(TimerId timer_id) { cancel(timer_id); }
  void kill_timer(std::uint64_t timer_id) { cancel(TimerId(timer_id)); }

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
    uint64_t generation;
    Clock::time_point when;
    Clock::duration interval;
    std::uint32_t repeat;
    TimerIdCallback callback;
  };

  TimerId add_timer(Clock::time_point when,
                    Clock::duration interval,
                    std::uint32_t repeat,
                    TimerIdCallback callback,
                    TimerId timer_id = {});
  [[nodiscard]] bool timer_is_current_locked(const Timer& timer) const;
  TimerId next_timer_id_locked();
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
  std::unordered_map<uint64_t, uint64_t> timer_generations_;
  uint64_t next_timer_id_{1};
  uint64_t next_timer_generation_{1};
};

}  // namespace oklib::net
