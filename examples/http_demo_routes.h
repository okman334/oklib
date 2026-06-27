#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "oklib/base/thread_pool.h"
#include "oklib/http/content_type.h"
#include "oklib/http/http_request.h"
#include "oklib/http/http_request_body_stream.h"
#include "oklib/http/http_response.h"
#include "oklib/http/http_response_writer.h"
#include "oklib/http/http_router.h"
#include "oklib/http/http_server.h"
#include "oklib/http/multipart_parser.h"

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
  response.add_header("Access-Control-Allow-Origin", "*");
  response.set_body(std::move(body));
  writer.send(std::move(response));
}

inline void add_upload_cors_headers(oklib::http::HttpResponse& response) {
  response.add_header("Access-Control-Allow-Origin", "*");
  response.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
  response.add_header("Access-Control-Allow-Headers", "Content-Type");
  response.add_header("Access-Control-Max-Age", "600");
}

inline bool is_safe_upload_file_name_byte(unsigned char value) {
  if (value >= 0x80) {
    return true;
  }
  if (value < 0x20 || value == 0x7f) {
    return false;
  }
  switch (static_cast<char>(value)) {
    case '/':
    case '\\':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|':
      return false;
    default:
      return true;
  }
}

inline std::string sanitized_upload_file_name(std::string file_name) {
  if (file_name.empty()) {
    file_name = "upload.bin";
  }

  std::string sanitized;
  sanitized.reserve(file_name.size());
  for (char ch : file_name) {
    const auto value = static_cast<unsigned char>(ch);
    if (is_safe_upload_file_name_byte(value)) {
      sanitized.push_back(ch);
    } else {
      sanitized.push_back('_');
    }
  }

  while (!sanitized.empty() && sanitized.front() == '.') {
    sanitized.erase(sanitized.begin());
  }
  if (sanitized.empty()) {
    sanitized = "upload.bin";
  }
  return sanitized;
}

inline void send_upload_json(oklib::http::HttpResponseWriter writer,
                             std::string_view file_name,
                             const std::filesystem::path& path,
                             std::size_t bytes,
                             std::string_view content_type) {
  auto response = writer.make_response();
  response.set_status_code(201);
  response.set_content_type("application/json; charset=utf-8");
  add_upload_cors_headers(response);
  response.set_body("{\"file\":\"" + json_escape(file_name) +
                    "\",\"path\":\"" + json_escape(path.string()) +
                    "\",\"content_type\":\"" + json_escape(content_type) +
                    "\",\"bytes\":" + std::to_string(bytes) + "}\n");
  writer.send(std::move(response));
}

inline void send_worker_upload_json(oklib::http::HttpResponseWriter writer,
                                    std::string_view file_name,
                                    const std::filesystem::path& path,
                                    std::size_t bytes,
                                    std::string_view content_type) {
  auto response = writer.make_response();
  response.set_status_code(201);
  response.set_content_type("application/json; charset=utf-8");
  response.add_header("X-Worker", "oklib-thread-pool");
  add_upload_cors_headers(response);
  response.set_body("{\"file\":\"" + json_escape(file_name) +
                    "\",\"path\":\"" + json_escape(path.string()) +
                    "\",\"content_type\":\"" + json_escape(content_type) +
                    "\",\"mode\":\"worker\"" +
                    ",\"bytes\":" + std::to_string(bytes) + "}\n");
  writer.send(std::move(response));
}

inline bool save_upload_body(const std::filesystem::path& upload_dir,
                             const std::filesystem::path& path,
                             std::string_view body) {
  std::error_code ec;
  std::filesystem::create_directories(upload_dir, ec);
  if (ec) {
    return false;
  }

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    return false;
  }
  file.write(body.data(), static_cast<std::streamsize>(body.size()));
  return static_cast<bool>(file);
}

struct WorkerUploadState {
  WorkerUploadState(std::filesystem::path upload_dir_arg,
                    std::filesystem::path path_arg,
                    std::string file_name_arg,
                    std::string content_type_arg,
                    oklib::http::HttpResponseWriter writer_arg)
      : upload_dir(std::move(upload_dir_arg)),
        path(std::move(path_arg)),
        file_name(std::move(file_name_arg)),
        content_type(std::move(content_type_arg)),
        writer(std::move(writer_arg)) {}

  void enqueue(std::string chunk) {
    {
      std::lock_guard lock(mutex);
      chunks.push_back(std::move(chunk));
    }
    cv.notify_one();
  }

  void finish() {
    {
      std::lock_guard lock(mutex);
      completed = true;
    }
    cv.notify_one();
  }

  void run() {
    std::error_code ec;
    std::filesystem::create_directories(upload_dir, ec);
    bool ok = !ec;

    std::ofstream file;
    if (ok) {
      file.open(path, std::ios::binary | std::ios::trunc);
      ok = file.is_open();
    }

    for (;;) {
      std::string chunk;
      {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return completed || !chunks.empty(); });
        if (!chunks.empty()) {
          chunk = std::move(chunks.front());
          chunks.pop_front();
        } else if (completed) {
          break;
        }
      }

      if (!ok) {
        continue;
      }

      file.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      bytes += chunk.size();
      if (!file) {
        ok = false;
      }
    }

    if (file.is_open()) {
      file.close();
      ok = ok && static_cast<bool>(file);
    }

    if (!ok) {
      send_text(std::move(writer), 500, "failed to save upload\n");
      return;
    }

    send_worker_upload_json(std::move(writer), file_name, path, bytes, content_type);
  }

  std::filesystem::path upload_dir;
  std::filesystem::path path;
  std::string file_name;
  std::string content_type;
  oklib::http::HttpResponseWriter writer;

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::string> chunks;
  bool completed{false};
  std::size_t bytes{0};
};

inline void install_http_demo_routes(oklib::http::HttpServer& server,
                                     oklib::ThreadPool& workers) {
  oklib::http::HttpRouter router;

  router.get("/", [](const oklib::http::HttpRequest&,
                     oklib::http::HttpResponseWriter writer) {
    send_text(std::move(writer), 200,
              "oklib HTTP demo\n"
              "routes: GET /ping, GET /headers, GET /query?name=oklib, POST /echo, "
              "POST /stream-upload, POST /upload-file?name=photo.jpg, "
              "POST /upload-file-worker?name=photo.jpg, "
              "GET /async, GET /chunks, GET /cache\n");
  });

  router.get("/ping",
             [](const oklib::http::HttpRequest&,
                oklib::http::HttpResponseWriter writer) {
               send_text(std::move(writer), 200, "pong\n");
             });

  router.get("/headers",
             [](const oklib::http::HttpRequest& request,
                oklib::http::HttpResponseWriter writer) {
               std::ostringstream body;
               body << "{"
                    << "\"method\":\"" << json_escape(request.method_string()) << "\","
                    << "\"target\":\"" << json_escape(request.target()) << "\","
                    << "\"peer\":\"" << json_escape(request.peer_address()) << "\","
                    << "\"user_agent\":\"" << json_escape(request.header("User-Agent")) << "\""
                    << "}\n";
               send_text(std::move(writer), 200, body.str(), "application/json; charset=utf-8");
             });

  router.get("/query",
             [](const oklib::http::HttpRequest& request,
                oklib::http::HttpResponseWriter writer) {
               std::ostringstream body;
               body << "path=" << request.path() << "\nquery=" << request.query() << "\n";
               send_text(std::move(writer), 200, body.str());
             });

  router.get("/async",
             [&workers](const oklib::http::HttpRequest& request,
                        oklib::http::HttpResponseWriter writer) {
               workers.run([request, writer = std::move(writer)]() mutable {
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
             });

  router.get("/chunks",
             [&workers](const oklib::http::HttpRequest&,
                        oklib::http::HttpResponseWriter writer) {
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
             });

  router.get("/cache",
             [](const oklib::http::HttpRequest& request,
                oklib::http::HttpResponseWriter writer) {
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
             });

  auto upload_handler = [](oklib::http::HttpRequest request,
                           oklib::http::HttpRequestBodyStream body_stream,
                           oklib::http::HttpResponseWriter writer) {
    auto body = std::make_shared<std::string>();
    auto total_bytes = std::make_shared<std::size_t>(0);
    const bool stream_only = request.path() == "/stream-upload";
    body_stream.set_data_callback([body, total_bytes, stream_only](std::string_view chunk) {
      *total_bytes += chunk.size();
      if (!stream_only) {
        body->append(chunk);
      }
    });
    body_stream.set_complete_callback(
        [path = request.path(), body, total_bytes, writer = std::move(writer)]() mutable {
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
  };

  router.post_streaming("/echo", upload_handler);
  router.put_streaming("/echo", upload_handler);
  router.post_streaming("/stream-upload", upload_handler);
  router.put_streaming("/stream-upload", upload_handler);

  router.options("/upload-file",
                 [](const oklib::http::HttpRequest&,
                    oklib::http::HttpResponseWriter writer) {
                   auto response = writer.make_response();
                   response.set_status_code(204);
                   add_upload_cors_headers(response);
                   writer.send(std::move(response));
                 });

  router.options("/upload-file-worker",
                 [](const oklib::http::HttpRequest&,
                    oklib::http::HttpResponseWriter writer) {
                   auto response = writer.make_response();
                   response.set_status_code(204);
                   add_upload_cors_headers(response);
                   writer.send(std::move(response));
                 });

  router.post_streaming("/upload-file",
                        [](oklib::http::HttpRequest request,
                           oklib::http::HttpRequestBodyStream body_stream,
                           oklib::http::HttpResponseWriter writer) {
                          const auto upload_dir = std::make_shared<std::filesystem::path>("uploads");
                          const auto request_content_type = request.header("Content-Type");
                          if (oklib::http::content_type_from_string(request_content_type) ==
                              oklib::http::ContentType::multipart_form_data) {
                            auto body = std::make_shared<std::string>();
                            body_stream.set_data_callback([body](std::string_view chunk) {
                              body->append(chunk);
                            });
                            body_stream.set_complete_callback(
                                [body,
                                 upload_dir,
                                 request_content_type,
                                 fallback_name = request.query_param("name").value_or(std::string{}),
                                 writer = std::move(writer)]() mutable {
                                  const auto parsed = oklib::http::parse_multipart_form_data(
                                      request_content_type, *body);
                                  if (!parsed.ok()) {
                                    send_text(std::move(writer), 400, "malformed multipart upload\n");
                                    return;
                                  }

                                  const oklib::http::MultipartPart* file_part = nullptr;
                                  if (const auto* named_file = parsed.find("file");
                                      named_file != nullptr && named_file->is_file()) {
                                    file_part = named_file;
                                  } else {
                                    for (const auto& part : parsed.parts) {
                                      if (part.is_file()) {
                                        file_part = &part;
                                        break;
                                      }
                                    }
                                  }
                                  if (file_part == nullptr) {
                                    send_text(std::move(writer), 400, "missing multipart file part\n");
                                    return;
                                  }

                                  auto file_name = sanitized_upload_file_name(
                                      file_part->filename.empty() ? fallback_name : file_part->filename);
                                  auto path = *upload_dir / file_name;
                                  if (!save_upload_body(*upload_dir, path, file_part->body)) {
                                    send_text(std::move(writer), 500, "failed to save upload\n");
                                    return;
                                  }

                                  send_upload_json(std::move(writer),
                                                   file_name,
                                                   path,
                                                   file_part->body.size(),
                                                   file_part->content_type);
                                });
                            return;
                          }

                          const auto file_name = std::make_shared<std::string>(
                              sanitized_upload_file_name(
                                  request.query_param("name").value_or(std::string{})));
                          const auto path = std::make_shared<std::filesystem::path>(
                              *upload_dir / *file_name);
                          const auto bytes = std::make_shared<std::size_t>(0);
                          const auto ok = std::make_shared<bool>(true);
                          const auto file = std::make_shared<std::ofstream>();

                          std::error_code ec;
                          std::filesystem::create_directories(*upload_dir, ec);
                          if (ec) {
                            *ok = false;
                          } else {
                            file->open(*path, std::ios::binary | std::ios::trunc);
                            *ok = file->is_open();
                          }

                          body_stream.set_data_callback([file, bytes, ok](std::string_view chunk) {
                            if (!*ok) {
                              return;
                            }
                            file->write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                            *bytes += chunk.size();
                            if (!*file) {
                              *ok = false;
                            }
                          });
                          body_stream.set_complete_callback(
                              [file_name,
                               path,
                               bytes,
                               ok,
                               file,
                               request_content_type,
                               writer = std::move(writer)]() mutable {
                                if (file->is_open()) {
                                  file->close();
                                }

                                if (!*ok) {
                                  send_text(std::move(writer), 500, "failed to save upload\n");
                                  return;
                                }

                                send_upload_json(std::move(writer),
                                                 *file_name,
                                                 *path,
                                                 *bytes,
                                                 request_content_type);
                              });
                        });

  router.post_streaming("/upload-file-worker",
                        [&workers](oklib::http::HttpRequest request,
                                   oklib::http::HttpRequestBodyStream body_stream,
                                   oklib::http::HttpResponseWriter writer) {
                          if (oklib::http::content_type_from_string(request.header("Content-Type")) ==
                              oklib::http::ContentType::multipart_form_data) {
                            send_text(std::move(writer),
                                      415,
                                      "worker upload demo supports raw request bodies only\n");
                            return;
                          }

                          const auto upload_dir = std::filesystem::path("uploads");
                          auto file_name = sanitized_upload_file_name(
                              request.query_param("name").value_or(std::string{}));
                          auto path = upload_dir / file_name;
                          auto state = std::make_shared<WorkerUploadState>(
                              upload_dir,
                              path,
                              std::move(file_name),
                              request.header("Content-Type"),
                              std::move(writer));

                          workers.run([state] { state->run(); });
                          body_stream.set_data_callback([state](std::string_view chunk) {
                            state->enqueue(std::string(chunk));
                          });
                          body_stream.set_complete_callback([state] {
                            state->finish();
                          });
                        });

  server.set_router(router);
}

}  // namespace oklib::examples
