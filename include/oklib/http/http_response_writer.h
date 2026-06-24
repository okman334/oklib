#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

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
  bool start_chunked(HttpResponse response) const;
  bool write_chunk(std::string_view data) const;
  bool finish() const;

 private:
  struct State;
  using StatePtr = std::shared_ptr<State>;

  friend class HttpContext;
  friend class HttpServer;

  static StatePtr make_state(const oklib::net::TcpConnectionPtr& connection);
  static void flush_ready_responses(const StatePtr& state);
  static std::string encode_chunk(std::string_view data);

  HttpResponseWriter(StatePtr state, std::size_t sequence, bool close_connection, bool include_body);
  bool enqueue(std::string bytes, bool finished, bool close_connection) const;

  StatePtr state_;
  std::size_t sequence_{0};
  bool close_connection_{false};
  bool include_body_{true};
  std::shared_ptr<std::atomic<int>> write_state_;
};

}  // namespace oklib::http
