#include "oklib/http/http_request_body_stream.h"

#include <mutex>
#include <utility>

namespace oklib::http {

struct HttpRequestBodyStream::State {
  std::mutex mutex;
  DataCallback data_callback;
  CompleteCallback complete_callback;
  CancelCallback cancel_callback;
  ControlCallback pause_callback;
  ControlCallback resume_callback;
  std::atomic_bool completed{false};
  std::atomic_bool canceled{false};
  std::atomic_bool reading_paused{false};
};

HttpRequestBodyStream::HttpRequestBodyStream()
    : state_(std::make_shared<State>()) {}

void HttpRequestBodyStream::set_data_callback(DataCallback callback) const {
  if (!state_) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  state_->data_callback = std::move(callback);
}

void HttpRequestBodyStream::set_complete_callback(CompleteCallback callback) const {
  if (!state_) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  state_->complete_callback = std::move(callback);
}

void HttpRequestBodyStream::set_cancel_callback(CancelCallback callback) const {
  if (!state_) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  state_->cancel_callback = std::move(callback);
}

void HttpRequestBodyStream::pause_reading() const {
  if (!state_) {
    return;
  }
  bool expected = false;
  if (!state_->reading_paused.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }

  ControlCallback callback;
  {
    std::lock_guard lock(state_->mutex);
    callback = state_->pause_callback;
  }
  if (callback) {
    callback();
  }
}

void HttpRequestBodyStream::resume_reading() const {
  if (!state_) {
    return;
  }
  bool expected = true;
  if (!state_->reading_paused.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
    return;
  }

  ControlCallback callback;
  {
    std::lock_guard lock(state_->mutex);
    callback = state_->resume_callback;
  }
  if (callback) {
    callback();
  }
}

bool HttpRequestBodyStream::reading_paused() const noexcept {
  return state_ != nullptr && state_->reading_paused.load(std::memory_order_acquire);
}

void HttpRequestBodyStream::on_data(std::string_view chunk) const {
  if (!state_ || chunk.empty()) {
    return;
  }
  if (state_->canceled.load(std::memory_order_acquire) ||
      state_->completed.load(std::memory_order_acquire)) {
    return;
  }

  DataCallback callback;
  {
    std::lock_guard lock(state_->mutex);
    callback = state_->data_callback;
  }
  if (callback) {
    callback(chunk);
  }
}

void HttpRequestBodyStream::on_complete() const {
  if (!state_) {
    return;
  }
  if (state_->canceled.load(std::memory_order_acquire)) {
    return;
  }

  bool expected = false;
  if (!state_->completed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }

  CompleteCallback callback;
  {
    std::lock_guard lock(state_->mutex);
    callback = std::move(state_->complete_callback);
    state_->data_callback = {};
    state_->cancel_callback = {};
    state_->pause_callback = {};
    state_->resume_callback = {};
  }
  if (callback) {
    callback();
  }
}

void HttpRequestBodyStream::on_cancel() const {
  if (!state_) {
    return;
  }
  bool expected = false;
  if (!state_->canceled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }
  if (state_->completed.load(std::memory_order_acquire)) {
    return;
  }

  CancelCallback callback;
  {
    std::lock_guard lock(state_->mutex);
    callback = std::move(state_->cancel_callback);
    state_->data_callback = {};
    state_->complete_callback = {};
    state_->pause_callback = {};
    state_->resume_callback = {};
  }
  if (callback) {
    callback();
  }
}

void HttpRequestBodyStream::set_pause_callback(ControlCallback callback) const {
  if (!state_) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  state_->pause_callback = std::move(callback);
}

void HttpRequestBodyStream::set_resume_callback(ControlCallback callback) const {
  if (!state_) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  state_->resume_callback = std::move(callback);
}

}  // namespace oklib::http
