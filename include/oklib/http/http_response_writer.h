#pragma once

#include <atomic>
#include <cstddef>
#include <memory>

#include "oklib/http/http_response.h"
#include "oklib/net/callbacks.h"

namespace oklib::http {

class HttpContext;
class HttpServer;

class HttpResponseWriter {
 public:
  HttpResponseWriter() = default;

  [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }
  [[nodiscard]] bool close_connection() const noexcept { return close_connection_; }
  [[nodiscard]] bool include_body() const noexcept { return include_body_; }

  [[nodiscard]] HttpResponse make_response() const { return HttpResponse(close_connection_); }

  bool send(HttpResponse response) const;

 private:
  struct State;
  using StatePtr = std::shared_ptr<State>;

  friend class HttpContext;
  friend class HttpServer;

  static StatePtr make_state(const oklib::net::TcpConnectionPtr& connection);
  static void flush_ready_responses(const StatePtr& state);

  HttpResponseWriter(StatePtr state, std::size_t sequence, bool close_connection, bool include_body);

  StatePtr state_;
  std::size_t sequence_{0};
  bool close_connection_{false};
  bool include_body_{true};
  std::shared_ptr<std::atomic_bool> sent_;
};

}  // namespace oklib::http
