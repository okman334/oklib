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

  HttpRequestBodyStream();

  void set_data_callback(DataCallback callback) const;
  void set_complete_callback(CompleteCallback callback) const;

 private:
  struct State;

  friend class HttpContext;
  friend class HttpServer;

  void on_data(std::string_view chunk) const;
  void on_complete() const;

  std::shared_ptr<State> state_;
};

}  // namespace oklib::http
