#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "oklib/base/noncopyable.h"
#include "oklib/http/http_headers.h"
#include "oklib/http/http_parser.h"
#include "oklib/http/tls_options.h"
#include "oklib/net/tcp_client.h"

namespace oklib::http {

class HttpClientCache;

struct HttpClientOptions {
  bool retry{false};
  std::shared_ptr<HttpClientCache> cache;
  TlsClientOptions tls;
};

class HttpClientRequest {
 public:
  HttpClientRequest(std::string method = "GET", std::string target = "/");

  [[nodiscard]] const std::string& method() const noexcept { return method_; }
  [[nodiscard]] const std::string& target() const noexcept { return target_; }
  [[nodiscard]] const std::string& url() const noexcept { return url_; }
  [[nodiscard]] const std::string& body() const noexcept { return body_; }
  [[nodiscard]] const HttpHeaders& headers() const noexcept { return headers_; }
  [[nodiscard]] std::chrono::milliseconds timeout() const noexcept { return timeout_; }

  void set_method(std::string method) { method_ = std::move(method); }
  void set_target(std::string target) { target_ = std::move(target); }
  void set_url(std::string url) { url_ = std::move(url); }
  void set_body(std::string body) { body_ = std::move(body); }
  void set_timeout(std::chrono::milliseconds timeout) { timeout_ = timeout; }
  void add_header(std::string_view field, std::string_view value) { headers_.add(field, value); }
  void set_header(std::string_view field, std::string_view value) { headers_.set(field, value); }

  [[nodiscard]] std::string serialize(std::string_view host, bool include_body = true) const;

 private:
  std::string method_;
  std::string target_;
  std::string url_;
  HttpHeaders headers_;
  std::string body_;
  std::chrono::milliseconds timeout_{0};
};

class HttpClientResponseStream {
 public:
  using DataCallback = std::function<void(std::string_view)>;
  using CompleteCallback = std::function<void()>;

  HttpClientResponseStream();

  void set_data_callback(DataCallback callback) const;
  void set_complete_callback(CompleteCallback callback) const;

 private:
  struct State;
  friend class HttpClient;

  void on_data(std::string_view chunk) const;
  void on_complete() const;

  std::shared_ptr<State> state_;
};

class HttpClient : private oklib::Noncopyable {
 public:
  using ResponseCallback = std::function<void(HttpResponseMessage)>;
  using StreamingResponseCallback =
      std::function<void(HttpResponseMessage, HttpClientResponseStream)>;
  using ErrorCallback = std::function<void()>;

  HttpClient(oklib::net::EventLoop* loop,
             const oklib::net::InetAddress& server_address,
             std::string host,
             std::string name,
             HttpClientOptions options = {});
  HttpClient(oklib::net::EventLoop* loop,
             std::vector<oklib::net::InetAddress> server_addresses,
             std::string host,
             std::string name,
             HttpClientOptions options = {});
  ~HttpClient();

  void connect();
  void disconnect();
  void stop();
  void send(HttpClientRequest request);

  void set_response_callback(ResponseCallback callback) { response_callback_ = std::move(callback); }
  void set_streaming_response_callback(StreamingResponseCallback callback) {
    streaming_response_callback_ = std::move(callback);
  }
  void set_error_callback(ErrorCallback callback) { error_callback_ = std::move(callback); }

 private:
  enum class StreamingBodyMode {
    none,
    fixed_length,
    chunked,
  };

  enum class StreamingChunkState {
    size,
    data,
    data_crlf,
    trailers,
  };

  struct QueuedRequest {
    HttpClientRequest request;
    std::optional<std::size_t> cache_entry_id;
  };

  void on_connection(const oklib::net::TcpConnectionPtr& connection);
  void on_message(const oklib::net::TcpConnectionPtr& connection,
                  oklib::net::Buffer* buffer,
                  oklib::Timestamp receive_time);
  void send_in_loop(HttpClientRequest request);
  void request_connect();
  void flush_requests();
  void handle_continue_response();
  HttpParseStatus process_streaming_body(oklib::net::Buffer* buffer);
  HttpParseStatus process_fixed_streaming_body(oklib::net::Buffer* buffer);
  HttpParseStatus process_chunked_streaming_body(oklib::net::Buffer* buffer);
  void finish_streaming_response();
  void handle_parse_error();
  void deliver_cached_response(HttpResponseMessage response);
  [[nodiscard]] bool response_closes_connection(const HttpResponseMessage& response) const;

  oklib::net::EventLoop* loop_;
  std::string host_;
  oklib::net::TcpClient client_;
  std::shared_ptr<HttpClientCache> cache_;
  TlsClientOptions tls_options_;
  ResponseCallback response_callback_;
  StreamingResponseCallback streaming_response_callback_;
  ErrorCallback error_callback_;
  HttpParser parser_{HttpParserMode::response};
  oklib::net::TcpConnectionPtr connection_;
  std::deque<QueuedRequest> pending_requests_;
  std::deque<QueuedRequest> in_flight_requests_;
  HttpClientResponseStream response_stream_;
  StreamingBodyMode streaming_body_mode_{StreamingBodyMode::none};
  StreamingChunkState streaming_chunk_state_{StreamingChunkState::size};
  std::uint64_t streaming_body_remaining_{0};
  std::uint64_t streaming_decoded_body_bytes_{0};
  std::size_t streaming_trailer_bytes_{0};
  bool streaming_response_active_{false};
  bool streaming_response_close_{false};
  bool awaiting_continue_{false};
  bool connecting_{false};
  bool stopped_{false};
};

}  // namespace oklib::http
