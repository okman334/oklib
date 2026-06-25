#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "oklib/http/http_request.h"
#include "oklib/http/http_request_body_stream.h"
#include "oklib/http/http_response_writer.h"
#include "oklib/http/http_server.h"

namespace oklib::http {

class HttpRouter {
 public:
  using Handler = std::function<void(const HttpRequest&, HttpResponseWriter)>;
  using StreamingHandler =
      std::function<void(HttpRequest, HttpRequestBodyStream, HttpResponseWriter)>;
  using NotFoundHandler = std::function<void(const HttpRequest&, HttpResponseWriter)>;
  using MethodNotAllowedHandler =
      std::function<void(const HttpRequest&, const std::vector<std::string>&, HttpResponseWriter)>;

  HttpRouter();

  void add(std::string method, std::string path, Handler handler);
  void add_streaming(std::string method, std::string path, StreamingHandler handler);

  void get(std::string path, Handler handler) { add("GET", std::move(path), std::move(handler)); }
  void head(std::string path, Handler handler) { add("HEAD", std::move(path), std::move(handler)); }
  void post(std::string path, Handler handler) { add("POST", std::move(path), std::move(handler)); }
  void put(std::string path, Handler handler) { add("PUT", std::move(path), std::move(handler)); }
  void delete_(std::string path, Handler handler) {
    add("DELETE", std::move(path), std::move(handler));
  }
  void patch(std::string path, Handler handler) { add("PATCH", std::move(path), std::move(handler)); }
  void options(std::string path, Handler handler) {
    add("OPTIONS", std::move(path), std::move(handler));
  }

  void post_streaming(std::string path, StreamingHandler handler) {
    add_streaming("POST", std::move(path), std::move(handler));
  }
  void put_streaming(std::string path, StreamingHandler handler) {
    add_streaming("PUT", std::move(path), std::move(handler));
  }
  void patch_streaming(std::string path, StreamingHandler handler) {
    add_streaming("PATCH", std::move(path), std::move(handler));
  }

  void set_not_found_handler(NotFoundHandler handler);
  void set_method_not_allowed_handler(MethodNotAllowedHandler handler);

  [[nodiscard]] HttpServer::StreamingHttpCallback streaming_callback() const;

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace oklib::http
