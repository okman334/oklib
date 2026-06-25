#pragma once

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "oklib/base/thread_pool.h"
#include "oklib/http/http_request.h"
#include "oklib/http/http_request_body_stream.h"
#include "oklib/http/http_response.h"
#include "oklib/http/http_response_writer.h"
#include "oklib/http/http_server.h"

namespace oklib::examples {

inline std::string json_escape(std::string_view input) {
  std::string output;
  output.reserve(input.size() + 8);
  for (char ch : input) {
    switch (ch) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output.push_back(ch);
        break;
    }
  }
  return output;
}

inline void send_text(oklib::http::HttpResponseWriter writer,
                      int status,
                      std::string body,
                      std::string content_type = "text/plain; charset=utf-8") {
  auto response = writer.make_response();
  response.set_status_code(status);
  response.set_content_type(std::move(content_type));
  response.set_body(std::move(body));
  writer.send(std::move(response));
}

inline void install_http_demo_routes(oklib::http::HttpServer& server,
                                     oklib::ThreadPool& workers) {
  server.set_streaming_http_callback(
      [&workers](oklib::http::HttpRequest request,
                 oklib::http::HttpRequestBodyStream body_stream,
                 oklib::http::HttpResponseWriter writer) mutable {
        if (request.path() == "/") {
          send_text(std::move(writer), 200,
                    "oklib HTTP demo\n"
                    "routes: /headers, /query?name=oklib, /echo, /stream-upload, "
                    "/async, /chunks, /cache\n");
          return;
        }

        if (request.path() == "/headers") {
          std::ostringstream body;
          body << "{"
               << "\"method\":\"" << json_escape(request.method_string()) << "\","
               << "\"target\":\"" << json_escape(request.target()) << "\","
               << "\"peer\":\"" << json_escape(request.peer_address()) << "\","
               << "\"user_agent\":\"" << json_escape(request.header("User-Agent")) << "\""
               << "}\n";
          send_text(std::move(writer), 200, body.str(), "application/json; charset=utf-8");
          return;
        }

        if (request.path() == "/query") {
          std::ostringstream body;
          body << "path=" << request.path() << "\nquery=" << request.query() << "\n";
          send_text(std::move(writer), 200, body.str());
          return;
        }

        if (request.path() == "/async") {
          workers.run([request = std::move(request), writer = std::move(writer)]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::ostringstream body;
            body << "async worker completed for " << request.target() << "\n";
            auto response = writer.make_response();
            response.set_status_code(200);
            response.set_content_type("text/plain; charset=utf-8");
            response.add_header("X-Worker", "oklib-thread-pool");
            response.set_body(body.str());
            writer.send(std::move(response));
          });
          return;
        }

        if (request.path() == "/chunks") {
          workers.run([writer = std::move(writer)]() mutable {
            auto response = writer.make_response();
            response.set_status_code(200);
            response.set_content_type("text/plain; charset=utf-8");
            response.add_header("X-Stream", "chunked");
            if (!writer.start_chunked(std::move(response))) {
              return;
            }
            for (int i = 1; i <= 5; ++i) {
              writer.write_chunk("chunk " + std::to_string(i) + "\n");
              std::this_thread::sleep_for(std::chrono::milliseconds(80));
            }
            writer.finish();
          });
          return;
        }

        if (request.path() == "/cache") {
          const std::string etag = "\"oklib-demo-v1\"";
          auto response = writer.make_response();
          response.add_header("Cache-Control", "max-age=30");
          response.add_header("ETag", etag);
          response.add_header("Last-Modified", "Thu, 25 Jun 2026 00:00:00 GMT");
          if (request.header("If-None-Match") == etag) {
            response.set_status_code(304);
            writer.send(std::move(response));
            return;
          }
          response.set_status_code(200);
          response.set_content_type("text/plain; charset=utf-8");
          response.set_body("cacheable response\n");
          writer.send(std::move(response));
          return;
        }

        if (request.path() == "/echo" || request.path() == "/stream-upload") {
          auto body = std::make_shared<std::string>();
          auto total_bytes = std::make_shared<std::size_t>(0);
          body_stream.set_data_callback([body, total_bytes, stream_only = request.path() == "/stream-upload"](
                                            std::string_view chunk) {
            *total_bytes += chunk.size();
            if (!stream_only) {
              body->append(chunk);
            }
          });
          body_stream.set_complete_callback(
              [method = request.method_string(),
               path = request.path(),
               body,
               total_bytes,
               writer = std::move(writer)]() mutable {
                if (method != "POST" && method != "PUT") {
                  send_text(std::move(writer), 405, "method not allowed\n");
                  return;
                }
                auto response = writer.make_response();
                response.set_status_code(200);
                response.set_content_type("text/plain; charset=utf-8");
                if (path == "/stream-upload") {
                  response.set_body("streamed bytes=" + std::to_string(*total_bytes) + "\n");
                } else {
                  response.set_body(*body);
                }
                writer.send(std::move(response));
              });
          return;
        }

        send_text(std::move(writer), 404, "not found\n");
      });
}

}  // namespace oklib::examples
