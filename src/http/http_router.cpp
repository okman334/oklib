#include "oklib/http/http_router.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

#include "oklib/http/http_response.h"

namespace oklib::http {
namespace {

std::string uppercase(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return value;
}

void send_plain(HttpResponseWriter writer, int status, std::string body) {
  auto response = writer.make_response();
  response.set_status_code(status);
  response.set_content_type("text/plain; charset=utf-8");
  response.set_body(std::move(body));
  writer.send(std::move(response));
}

std::string join_methods(const std::vector<std::string>& methods) {
  std::string result;
  for (const auto& method : methods) {
    if (!result.empty()) {
      result += ", ";
    }
    result += method;
  }
  return result;
}

bool has_streamed_body(const HttpRequest& request) {
  return (request.has_content_length() && request.content_length() > 0) ||
         !request.header("Transfer-Encoding").empty();
}

}  // namespace

struct HttpRouter::State {
  struct Route {
    std::string method;
    std::string path;
    StreamingHandler handler;
  };

  std::vector<Route> routes;
  NotFoundHandler not_found_handler;
  MethodNotAllowedHandler method_not_allowed_handler;

  [[nodiscard]] const Route* find_route(std::string_view method, std::string_view path) const {
    for (const auto& route : routes) {
      if (route.method == method && route.path == path) {
        return &route;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::vector<std::string> allowed_methods(std::string_view path) const {
    std::vector<std::string> methods;
    for (const auto& route : routes) {
      if (route.path != path) {
        continue;
      }
      if (std::find(methods.begin(), methods.end(), route.method) == methods.end()) {
        methods.push_back(route.method);
      }
    }
    return methods;
  }
};

HttpRouter::HttpRouter() : state_(std::make_shared<State>()) {
  state_->not_found_handler = [](const HttpRequest&, HttpResponseWriter writer) {
    send_plain(std::move(writer), 404, "not found\n");
  };
  state_->method_not_allowed_handler =
      [](const HttpRequest&,
         const std::vector<std::string>& methods,
         HttpResponseWriter writer) {
        auto response = writer.make_response();
        response.set_status_code(405);
        response.set_content_type("text/plain; charset=utf-8");
        response.add_header("Allow", join_methods(methods));
        response.set_body("method not allowed\n");
        writer.send(std::move(response));
      };
}

void HttpRouter::add(std::string method, std::string path, Handler handler) {
  add_streaming(std::move(method),
                std::move(path),
                [handler = std::move(handler)](HttpRequest request,
                                                HttpRequestBodyStream body,
                                                HttpResponseWriter writer) mutable {
                  if (has_streamed_body(request)) {
                    auto buffered_request = std::make_shared<HttpRequest>(std::move(request));
                    auto buffered_body = std::make_shared<std::string>();
                    body.set_data_callback([buffered_body](std::string_view chunk) {
                      buffered_body->append(chunk);
                    });
                    body.set_complete_callback([handler,
                                                buffered_request,
                                                buffered_body,
                                                writer = std::move(writer)]() mutable {
                      buffered_request->set_body(std::move(*buffered_body));
                      handler(*buffered_request, std::move(writer));
                    });
                    return;
                  }
                  handler(request, std::move(writer));
                });
}

void HttpRouter::add_streaming(std::string method,
                               std::string path,
                               StreamingHandler handler) {
  method = uppercase(std::move(method));
  auto it = std::find_if(state_->routes.begin(),
                         state_->routes.end(),
                         [&](const State::Route& route) {
                           return route.method == method && route.path == path;
                         });
  if (it != state_->routes.end()) {
    it->handler = std::move(handler);
    return;
  }
  state_->routes.push_back({std::move(method), std::move(path), std::move(handler)});
}

void HttpRouter::set_not_found_handler(NotFoundHandler handler) {
  state_->not_found_handler = std::move(handler);
}

void HttpRouter::set_method_not_allowed_handler(MethodNotAllowedHandler handler) {
  state_->method_not_allowed_handler = std::move(handler);
}

HttpServer::StreamingHttpCallback HttpRouter::streaming_callback() const {
  auto state = state_;
  return [state = std::move(state)](HttpRequest request,
                                    HttpRequestBodyStream body,
                                    HttpResponseWriter writer) mutable {
    const auto method = uppercase(request.method_string());
    if (const auto* route = state->find_route(method, request.path()); route != nullptr) {
      route->handler(std::move(request), std::move(body), std::move(writer));
      return;
    }

    auto methods = state->allowed_methods(request.path());
    if (!methods.empty()) {
      state->method_not_allowed_handler(request, methods, std::move(writer));
      return;
    }

    state->not_found_handler(request, std::move(writer));
  };
}

}  // namespace oklib::http
