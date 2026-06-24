#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "oklib/base/timestamp.h"
#include "oklib/http/http_headers.h"

namespace oklib::http {

enum class HttpMethod {
  invalid,
  get,
  head,
  post,
  put,
  delete_,
  options,
  trace,
  connect,
  patch,
};

enum class HttpVersion {
  unknown,
  http10,
  http11,
};

enum class HttpRequestTargetForm {
  unknown,
  origin,
  absolute,
  authority,
  asterisk,
};

class HttpRequest {
 public:
  [[nodiscard]] HttpMethod method() const noexcept { return method_; }
  [[nodiscard]] const std::string& method_token() const noexcept { return method_token_; }
  [[nodiscard]] std::string method_string() const;
  [[nodiscard]] HttpVersion version() const noexcept { return version_; }
  [[nodiscard]] HttpRequestTargetForm target_form() const noexcept { return target_form_; }
  [[nodiscard]] const std::string& target() const noexcept { return target_; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] const std::string& query() const noexcept { return query_; }
  [[nodiscard]] const std::string& body() const noexcept { return body_; }
  [[nodiscard]] bool has_content_length() const noexcept { return content_length_.has_value(); }
  [[nodiscard]] std::uint64_t content_length() const noexcept { return content_length_.value_or(0); }
  [[nodiscard]] oklib::Timestamp receive_time() const noexcept { return receive_time_; }
  [[nodiscard]] const std::string& peer_ip() const noexcept { return peer_ip_; }
  [[nodiscard]] uint16_t peer_port() const noexcept { return peer_port_; }
  [[nodiscard]] std::string peer_address() const {
    return peer_ip_.empty() ? std::string{} : peer_ip_ + ":" + std::to_string(peer_port_);
  }

  bool set_method(std::string_view method);
  void set_version(HttpVersion version) noexcept { version_ = version; }
  void set_target(std::string_view target);
  void set_target_form(HttpRequestTargetForm form) noexcept { target_form_ = form; }
  void set_path(std::string_view path) { path_.assign(path); }
  void set_query(std::string_view query) { query_.assign(query); }
  void set_body(std::string body) { body_ = std::move(body); }
  void append_body(std::string_view body) { body_.append(body); }
  void set_content_length(std::uint64_t length) noexcept { content_length_ = length; }
  void clear_content_length() noexcept { content_length_.reset(); }
  void set_receive_time(oklib::Timestamp receive_time) noexcept { receive_time_ = receive_time; }
  void set_peer_address(std::string_view ip, uint16_t port) {
    peer_ip_.assign(ip);
    peer_port_ = port;
  }

  void add_header(std::string_view field, std::string_view value);
  [[nodiscard]] std::string header(std::string_view field) const;
  [[nodiscard]] const HttpHeaders& headers() const noexcept { return headers_; }
  [[nodiscard]] HttpHeaders& mutable_headers() noexcept { return headers_; }
  [[nodiscard]] const HttpHeaders& trailers() const noexcept { return trailers_; }
  [[nodiscard]] HttpHeaders& mutable_trailers() noexcept { return trailers_; }

 private:
  HttpMethod method_{HttpMethod::invalid};
  std::string method_token_;
  HttpVersion version_{HttpVersion::unknown};
  HttpRequestTargetForm target_form_{HttpRequestTargetForm::unknown};
  std::string target_;
  std::string path_;
  std::string query_;
  std::string body_;
  std::optional<std::uint64_t> content_length_;
  oklib::Timestamp receive_time_;
  std::string peer_ip_;
  uint16_t peer_port_{0};
  HttpHeaders headers_;
  HttpHeaders trailers_;
};

}  // namespace oklib::http
