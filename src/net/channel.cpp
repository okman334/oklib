#include "oklib/net/channel.h"

#include "oklib/net/event_loop.h"

namespace oklib::net {

Channel::Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {}

Channel::~Channel() = default;

void Channel::handle_event(oklib::Timestamp receive_time) {
  if ((revents_ & k_close_event) != 0 && (revents_ & k_read_event) == 0) {
    if (close_callback_) {
      close_callback_();
    }
  }
  if ((revents_ & k_error_event) != 0) {
    if (error_callback_) {
      error_callback_();
    }
  }
  if ((revents_ & k_read_event) != 0) {
    if (read_callback_) {
      read_callback_(receive_time);
    }
  }
  if ((revents_ & k_write_event) != 0) {
    if (write_callback_) {
      write_callback_();
    }
  }
}

void Channel::enable_reading() {
  events_ |= k_read_event;
  update();
}

void Channel::disable_reading() {
  events_ &= ~k_read_event;
  update();
}

void Channel::enable_writing() {
  events_ |= k_write_event;
  update();
}

void Channel::disable_writing() {
  events_ &= ~k_write_event;
  update();
}

void Channel::disable_all() {
  events_ = k_none_event;
  update();
}

void Channel::remove() {
  loop_->remove_channel(this);
  added_to_loop_ = false;
}

void Channel::update() {
  loop_->update_channel(this);
  added_to_loop_ = true;
}

}  // namespace oklib::net
