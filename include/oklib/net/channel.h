#pragma once

#include <functional>

#include "oklib/base/noncopyable.h"
#include "oklib/base/timestamp.h"

namespace oklib::net {

class EventLoop;

class Channel : private oklib::Noncopyable {
 public:
  using EventCallback = std::function<void()>;
  using ReadEventCallback = std::function<void(oklib::Timestamp)>;

  static constexpr int k_none_event = 0;
  static constexpr int k_read_event = 1 << 0;
  static constexpr int k_write_event = 1 << 1;
  static constexpr int k_close_event = 1 << 2;
  static constexpr int k_error_event = 1 << 3;

  Channel(EventLoop* loop, int fd);
  ~Channel();

  void handle_event(oklib::Timestamp receive_time);

  void set_read_callback(ReadEventCallback callback) { read_callback_ = std::move(callback); }
  void set_write_callback(EventCallback callback) { write_callback_ = std::move(callback); }
  void set_close_callback(EventCallback callback) { close_callback_ = std::move(callback); }
  void set_error_callback(EventCallback callback) { error_callback_ = std::move(callback); }

  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] int events() const noexcept { return events_; }
  [[nodiscard]] int revents() const noexcept { return revents_; }
  void set_revents(int revents) noexcept { revents_ = revents; }

  [[nodiscard]] bool is_none_event() const noexcept { return events_ == k_none_event; }
  [[nodiscard]] bool is_reading() const noexcept { return (events_ & k_read_event) != 0; }
  [[nodiscard]] bool is_writing() const noexcept { return (events_ & k_write_event) != 0; }
  [[nodiscard]] EventLoop* owner_loop() const noexcept { return loop_; }

  void enable_reading();
  void disable_reading();
  void enable_writing();
  void disable_writing();
  void disable_all();
  void remove();

 private:
  void update();

  EventLoop* loop_;
  const int fd_;
  int events_{k_none_event};
  int revents_{k_none_event};
  bool added_to_loop_{false};

  ReadEventCallback read_callback_;
  EventCallback write_callback_;
  EventCallback close_callback_;
  EventCallback error_callback_;
};

}  // namespace oklib::net
