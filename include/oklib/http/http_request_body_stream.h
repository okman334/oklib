#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>

namespace oklib::http {

class HttpContext;
class HttpServer;

class HttpRequestBodyStream {
 public:
  using DataCallback = std::function<void(std::string_view)>;
  using CompleteCallback = std::function<void()>;
  using ControlCallback = std::function<void()>;
  using CancelCallback = std::function<void()>;

  HttpRequestBodyStream();

  void set_data_callback(DataCallback callback) const;
  void set_complete_callback(CompleteCallback callback) const;
  void set_cancel_callback(CancelCallback callback) const;

  void pause_reading() const;
  void resume_reading() const;
  [[nodiscard]] bool reading_paused() const noexcept;

 private:
  struct State;

  friend class HttpContext;
  friend class HttpServer;

  void on_data(std::string_view chunk) const;
  void on_complete() const;
  void on_cancel() const;
  void set_pause_callback(ControlCallback callback) const;
  void set_resume_callback(ControlCallback callback) const;

  std::shared_ptr<State> state_;
};

}  // namespace oklib::http
