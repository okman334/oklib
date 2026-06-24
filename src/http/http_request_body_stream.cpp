#include "oklib/http/http_request_body_stream.h"

#include <mutex>
#include <utility>

namespace oklib::http {

struct HttpRequestBodyStream::State {
  std::mutex mutex;
  DataCallback data_callback;
  CompleteCallback complete_callback;
  std::atomic_bool completed{false};
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

void HttpRequestBodyStream::on_data(std::string_view chunk) const {
  if (!state_ || chunk.empty()) {
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

  bool expected = false;
  if (!state_->completed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }

  CompleteCallback callback;
  {
    std::lock_guard lock(state_->mutex);
    callback = state_->complete_callback;
  }
  if (callback) {
    callback();
  }
}

}  // namespace oklib::http
