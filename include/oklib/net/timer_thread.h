#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "oklib/base/noncopyable.h"
#include "oklib/net/timer_id.h"

namespace oklib::net {

class EventLoop;
class EventLoopThread;

class TimerThread : private oklib::Noncopyable {
 public:
  using TimerCallback = std::function<void(TimerId)>;
  static constexpr std::uint32_t k_infinite_repeats = std::numeric_limits<std::uint32_t>::max();

  explicit TimerThread(std::string name = {});
  ~TimerThread();

  template <typename Rep, typename Period>
  TimerId set_timeout(std::chrono::duration<Rep, Period> timeout, TimerCallback callback) {
    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    if (delay < std::chrono::milliseconds::zero()) {
      return {};
    }
    return schedule(delay, 1, std::move(callback));
  }

  template <typename Rep, typename Period>
  TimerId set_timeout(TimerId timer_id,
                      std::chrono::duration<Rep, Period> timeout,
                      TimerCallback callback) {
    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    if (delay < std::chrono::milliseconds::zero()) {
      return {};
    }
    return schedule(delay, 1, std::move(callback), timer_id);
  }

  template <typename Rep, typename Period>
  TimerId set_timeout(std::uint64_t timer_id,
                      std::chrono::duration<Rep, Period> timeout,
                      TimerCallback callback) {
    return set_timeout(TimerId(timer_id), timeout, std::move(callback));
  }

  template <typename Rep, typename Period>
  TimerId set_interval(std::chrono::duration<Rep, Period> interval, TimerCallback callback) {
    const auto repeat = std::chrono::duration_cast<std::chrono::milliseconds>(interval);
    if (repeat <= std::chrono::milliseconds::zero()) {
      return {};
    }
    return schedule(repeat, k_infinite_repeats, std::move(callback));
  }

  template <typename Rep, typename Period>
  TimerId set_interval(TimerId timer_id,
                       std::chrono::duration<Rep, Period> interval,
                       TimerCallback callback) {
    const auto repeat = std::chrono::duration_cast<std::chrono::milliseconds>(interval);
    if (repeat <= std::chrono::milliseconds::zero()) {
      return {};
    }
    return schedule(repeat, k_infinite_repeats, std::move(callback), timer_id);
  }

  template <typename Rep, typename Period>
  TimerId set_interval(std::uint64_t timer_id,
                       std::chrono::duration<Rep, Period> interval,
                       TimerCallback callback) {
    return set_interval(TimerId(timer_id), interval, std::move(callback));
  }

  void clear(TimerId timer_id);
  void clear_timeout(TimerId timer_id) { clear(timer_id); }
  void clear_interval(TimerId timer_id) { clear(timer_id); }
  void stop();

 private:
  TimerId schedule(std::chrono::milliseconds delay,
                   std::uint32_t repeat,
                   TimerCallback callback);
  TimerId schedule(std::chrono::milliseconds delay,
                   std::uint32_t repeat,
                   TimerCallback callback,
                   TimerId timer_id);

  mutable std::mutex mutex_;
  std::unique_ptr<EventLoopThread> thread_;
  EventLoop* loop_{nullptr};
};

class GlobalTimer {
 public:
  using TimerCallback = TimerThread::TimerCallback;

  template <typename Rep, typename Period>
  static TimerId set_timeout(std::chrono::duration<Rep, Period> timeout, TimerCallback callback) {
    return instance().set_timeout(timeout, std::move(callback));
  }

  template <typename Rep, typename Period>
  static TimerId set_timeout(TimerId timer_id,
                             std::chrono::duration<Rep, Period> timeout,
                             TimerCallback callback) {
    return instance().set_timeout(timer_id, timeout, std::move(callback));
  }

  template <typename Rep, typename Period>
  static TimerId set_timeout(std::uint64_t timer_id,
                             std::chrono::duration<Rep, Period> timeout,
                             TimerCallback callback) {
    return instance().set_timeout(timer_id, timeout, std::move(callback));
  }

  template <typename Rep, typename Period>
  static TimerId set_interval(std::chrono::duration<Rep, Period> interval, TimerCallback callback) {
    return instance().set_interval(interval, std::move(callback));
  }

  template <typename Rep, typename Period>
  static TimerId set_interval(TimerId timer_id,
                              std::chrono::duration<Rep, Period> interval,
                              TimerCallback callback) {
    return instance().set_interval(timer_id, interval, std::move(callback));
  }

  template <typename Rep, typename Period>
  static TimerId set_interval(std::uint64_t timer_id,
                              std::chrono::duration<Rep, Period> interval,
                              TimerCallback callback) {
    return instance().set_interval(timer_id, interval, std::move(callback));
  }

  static void clear(TimerId timer_id);
  static void clear_timeout(TimerId timer_id) { clear(timer_id); }
  static void clear_interval(TimerId timer_id) { clear(timer_id); }
  static void shutdown();

 private:
  static TimerThread& instance();
};

}  // namespace oklib::net
