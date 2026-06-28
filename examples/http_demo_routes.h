#pragma once

#include <atomic>
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
#include "oklib/http/streaming_multipart_parser.h"
#include "oklib/http/upload_file_writer.h"

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
  static auto upload_writer_pool = [] {
    auto pool = std::make_shared<oklib::http::UploadFileWriterPool>();
    pool->start(2);
    return pool;
  }();

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
               //send_text(std::move(writer), 200, "pong\n");
               auto response = writer.make_response();
               response.set_status_code(200);
               response.set_body("pong\n");
               writer.send(std::move(response));
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
                            const auto boundary = oklib::http::multipart_boundary(request_content_type);
                            if (!boundary.has_value()) {
                              send_text(std::move(writer), 400, "missing multipart boundary\n");
                              return;
                            }

                            auto writer_holder =
                                std::make_shared<oklib::http::HttpResponseWriter>(std::move(writer));
                            auto response_sent = std::make_shared<std::atomic_bool>(false);
                            auto fallback_name =
                                request.query_param("name").value_or(std::string{});

                            auto send_error_once =
                                [writer_holder, response_sent](int status, std::string body) mutable {
                                  bool expected = false;
                                  if (!response_sent->compare_exchange_strong(expected, true)) {
                                    return;
                                  }
                                  send_text(std::move(*writer_holder), status, std::move(body));
                                };

                            auto make_completion =
                                [writer_holder, response_sent](oklib::http::UploadFileWriterResult result) mutable {
                                  bool expected = false;
                                  if (!response_sent->compare_exchange_strong(expected, true)) {
                                    return;
                                  }
                                  if (!result.ok) {
                                    if (!result.canceled) {
                                      send_text(std::move(*writer_holder),
                                                500,
                                                result.error_message.empty()
                                                    ? "failed to save upload\n"
                                                    : result.error_message + "\n");
                                    }
                                    return;
                                  }
                                  send_upload_json(std::move(*writer_holder),
                                                   result.file_name,
                                                   result.path,
                                                   result.bytes,
                                                   result.content_type);
                                };

                            auto make_options =
                                [upload_dir](std::string file_name, std::string content_type) {
                                  oklib::http::UploadFileWriterOptions options;
                                  options.upload_dir = *upload_dir;
                                  options.file_name = sanitized_upload_file_name(std::move(file_name));
                                  options.content_type = std::move(content_type);
                                  return options;
                                };

                            auto session =
                                std::make_shared<std::shared_ptr<oklib::http::UploadFileWriterSession>>();
                            auto accepting_file = std::make_shared<bool>(false);
                            auto parser = std::make_shared<oklib::http::StreamingMultipartParser>(
                                *boundary,
                                [session,
                                 accepting_file,
                                 body_stream,
                                 fallback_name = std::move(fallback_name),
                                 make_completion,
                                 send_error_once,
                                 make_options](const oklib::http::StreamingMultipartPart& part) mutable {
                                  *accepting_file = false;
                                  if (*session || !part.is_file()) {
                                    return;
                                  }

                                  auto file_name = part.filename.empty() ? fallback_name : part.filename;
                                  auto content_type = part.content_type.empty()
                                                          ? std::string("application/octet-stream")
                                                          : part.content_type;
                                  *session = upload_writer_pool->create_session(
                                      make_options(std::move(file_name), std::move(content_type)),
                                      make_completion,
                                      [body_stream] { body_stream.pause_reading(); },
                                      [body_stream] { body_stream.resume_reading(); });
                                  if (!*session) {
                                    send_error_once(503, "upload writer is busy\n");
                                    body_stream.pause_reading();
                                    return;
                                  }
                                  *accepting_file = true;
                                },
                                [session, accepting_file, body_stream, send_error_once](std::string_view data) mutable {
                                  if (!*accepting_file || !*session) {
                                    return;
                                  }
                                  if (!(*session)->append(data)) {
                                    (*session)->cancel();
                                    body_stream.pause_reading();
                                    send_error_once(413, "upload rejected\n");
                                  }
                                });

                            body_stream.set_data_callback(
                                [parser, session, send_error_once](std::string_view chunk) mutable {
                                  if (!parser->append(chunk)) {
                                    if (*session) {
                                      (*session)->cancel();
                                    }
                                    send_error_once(400, "malformed multipart upload\n");
                                  }
                                });
                            body_stream.set_complete_callback(
                                [parser, session, send_error_once]() mutable {
                                  if (!parser->finish()) {
                                    if (*session) {
                                      (*session)->cancel();
                                    }
                                    send_error_once(400, "malformed multipart upload\n");
                                    return;
                                  }
                                  if (!*session) {
                                    send_error_once(400, "missing multipart file part\n");
                                    return;
                                  }
                                  (*session)->finish();
                                });
                            body_stream.set_cancel_callback([session] {
                              if (*session) {
                                (*session)->cancel();
                              }
                            });
                            return;
                          }

                          auto writer_holder =
                              std::make_shared<oklib::http::HttpResponseWriter>(std::move(writer));
                          auto response_sent = std::make_shared<std::atomic_bool>(false);

                          auto send_error_once =
                              [writer_holder, response_sent](int status, std::string body) mutable {
                                bool expected = false;
                                if (!response_sent->compare_exchange_strong(expected, true)) {
                                  return;
                                }
                                send_text(std::move(*writer_holder), status, std::move(body));
                              };

                          auto make_completion =
                              [writer_holder, response_sent](oklib::http::UploadFileWriterResult result) mutable {
                                bool expected = false;
                                if (!response_sent->compare_exchange_strong(expected, true)) {
                                  return;
                                }
                                if (!result.ok) {
                                  if (!result.canceled) {
                                    send_text(std::move(*writer_holder),
                                              500,
                                              result.error_message.empty()
                                                  ? "failed to save upload\n"
                                                  : result.error_message + "\n");
                                  }
                                  return;
                                }
                                send_upload_json(std::move(*writer_holder),
                                                 result.file_name,
                                                 result.path,
                                                 result.bytes,
                                                 result.content_type);
                              };

                          oklib::http::UploadFileWriterOptions options;
                          options.upload_dir = *upload_dir;
                          options.file_name = sanitized_upload_file_name(
                              request.query_param("name").value_or(std::string{}));
                          options.content_type = request_content_type;
                          auto session = upload_writer_pool->create_session(
                              std::move(options),
                              make_completion,
                              [body_stream] { body_stream.pause_reading(); },
                              [body_stream] { body_stream.resume_reading(); });
                          if (!session) {
                            send_error_once(503, "upload writer is busy\n");
                            body_stream.pause_reading();
                            return;
                          }

                          body_stream.set_data_callback(
                              [session, body_stream, send_error_once](std::string_view chunk) mutable {
                                if (!session->append(chunk)) {
                                  session->cancel();
                                  body_stream.pause_reading();
                                  send_error_once(413, "upload rejected\n");
                                }
                              });
                          body_stream.set_complete_callback([session] {
                            session->finish();
                          });
                          body_stream.set_cancel_callback([session] {
                            session->cancel();
                          });
                        });

  router.post_streaming("/upload-file-worker",
                        [](oklib::http::HttpRequest request,
                           oklib::http::HttpRequestBodyStream body_stream,
                           oklib::http::HttpResponseWriter writer) {
                          const auto upload_dir = std::filesystem::path("uploads");
                          auto writer_holder =
                              std::make_shared<oklib::http::HttpResponseWriter>(std::move(writer));
                          auto response_sent = std::make_shared<std::atomic_bool>(false);

                          auto send_error_once =
                              [writer_holder, response_sent](int status, std::string body) mutable {
                                bool expected = false;
                                if (!response_sent->compare_exchange_strong(expected, true)) {
                                  return;
                                }
                                send_text(std::move(*writer_holder), status, std::move(body));
                              };

                          auto make_completion =
                              [writer_holder, response_sent](oklib::http::UploadFileWriterResult result) mutable {
                                bool expected = false;
                                if (!response_sent->compare_exchange_strong(expected, true)) {
                                  return;
                                }
                                if (!result.ok) {
                                  if (!result.canceled) {
                                    send_text(std::move(*writer_holder),
                                              500,
                                              result.error_message.empty()
                                                  ? "failed to save upload\n"
                                                  : result.error_message + "\n");
                                  }
                                  return;
                                }
                                send_worker_upload_json(std::move(*writer_holder),
                                                        result.file_name,
                                                        result.path,
                                                        result.bytes,
                                                        result.content_type);
                              };

                          auto make_options =
                              [upload_dir](std::string file_name, std::string content_type) {
                                oklib::http::UploadFileWriterOptions options;
                                options.upload_dir = upload_dir;
                                options.file_name = sanitized_upload_file_name(std::move(file_name));
                                options.content_type = std::move(content_type);
                                options.overwrite_existing = true;
                                return options;
                              };

                          const auto request_content_type = request.header("Content-Type");
                          if (oklib::http::content_type_from_string(request_content_type) !=
                              oklib::http::ContentType::multipart_form_data) {
                            auto file_name = request.query_param("name").value_or(std::string{});
                            auto session = upload_writer_pool->create_session(
                                make_options(std::move(file_name), request_content_type),
                                make_completion,
                                [body_stream] { body_stream.pause_reading(); },
                                [body_stream] { body_stream.resume_reading(); });
                            if (!session) {
                              send_error_once(503, "upload writer is busy\n");
                              body_stream.pause_reading();
                              return;
                            }
                            body_stream.set_data_callback(
                                [session, body_stream, send_error_once](std::string_view chunk) mutable {
                                  if (!session->append(chunk)) {
                                    session->cancel();
                                    body_stream.pause_reading();
                                    send_error_once(413, "upload rejected\n");
                                  }
                                });
                            body_stream.set_complete_callback([session] {
                              session->finish();
                            });
                            body_stream.set_cancel_callback([session] {
                              session->cancel();
                            });
                            return;
                          }

                          const auto boundary = oklib::http::multipart_boundary(request_content_type);
                          if (!boundary.has_value()) {
                            send_error_once(400, "missing multipart boundary\n");
                            return;
                          }

                          auto session =
                              std::make_shared<std::shared_ptr<oklib::http::UploadFileWriterSession>>();
                          auto accepting_file = std::make_shared<bool>(false);
                          auto parser = std::make_shared<oklib::http::StreamingMultipartParser>(
                              *boundary,
                              [session,
                               accepting_file,
                               body_stream,
                               make_completion,
                               send_error_once,
                               make_options](const oklib::http::StreamingMultipartPart& part) mutable {
                                *accepting_file = false;
                                if (*session || !part.is_file()) {
                                  return;
                                }

                                auto file_name = part.filename.empty()
                                                     ? std::string("upload.bin")
                                                     : part.filename;
                                auto content_type = part.content_type.empty()
                                                        ? std::string("application/octet-stream")
                                                        : part.content_type;
                                *session = upload_writer_pool->create_session(
                                    make_options(std::move(file_name), std::move(content_type)),
                                    make_completion,
                                    [body_stream] { body_stream.pause_reading(); },
                                    [body_stream] { body_stream.resume_reading(); });
                                if (!*session) {
                                  send_error_once(503, "upload writer is busy\n");
                                  body_stream.pause_reading();
                                  return;
                                }
                                *accepting_file = true;
                              },
                              [session, accepting_file, body_stream, send_error_once](std::string_view data) mutable {
                                if (!*accepting_file || !*session) {
                                  return;
                                }
                                if (!(*session)->append(data)) {
                                  (*session)->cancel();
                                  body_stream.pause_reading();
                                  send_error_once(413, "upload rejected\n");
                                }
                              });

                          body_stream.set_data_callback(
                              [parser, session, send_error_once](std::string_view chunk) mutable {
                                if (!parser->append(chunk)) {
                                  if (*session) {
                                    (*session)->cancel();
                                  }
                                  send_error_once(400, "malformed multipart upload\n");
                                }
                              });
                          body_stream.set_complete_callback(
                              [parser, session, send_error_once]() mutable {
                                if (!parser->finish()) {
                                  if (*session) {
                                    (*session)->cancel();
                                  }
                                  send_error_once(400, "malformed multipart upload\n");
                                  return;
                                }
                                if (!*session) {
                                  send_error_once(400, "missing multipart file part\n");
                                  return;
                                }
                                (*session)->finish();
                              });
                          body_stream.set_cancel_callback([session] {
                            if (*session) {
                              (*session)->cancel();
                            }
                          });
                        });

  server.set_router(router);
}

}  // namespace oklib::examples
