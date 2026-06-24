#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "oklib/base/timestamp.h"

namespace oklib::http {

enum class HttpMethod {
  invalid,
  get,
  head,
  post,
  put,
  delete_,
};

enum class HttpVersion {
  unknown,
  http10,
  http11,
};

class HttpRequest {
 public:
  [[nodiscard]] HttpMethod method() const noexcept { return method_; }
  [[nodiscard]] std::string method_string() const;
  [[nodiscard]] HttpVersion version() const noexcept { return version_; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] const std::string& query() const noexcept { return query_; }
  [[nodiscard]] oklib::Timestamp receive_time() const noexcept { return receive_time_; }
  [[nodiscard]] const std::string& peer_ip() const noexcept { return peer_ip_; }
  [[nodiscard]] uint16_t peer_port() const noexcept { return peer_port_; }
  [[nodiscard]] std::string peer_address() const {
    return peer_ip_.empty() ? std::string{} : peer_ip_ + ":" + std::to_string(peer_port_);
  }

  bool set_method(std::string_view method);
  void set_version(HttpVersion version) noexcept { version_ = version; }
  void set_path(std::string_view path) { path_.assign(path); }
  void set_query(std::string_view query) { query_.assign(query); }
  void set_receive_time(oklib::Timestamp receive_time) noexcept { receive_time_ = receive_time; }
  void set_peer_address(std::string_view ip, uint16_t port) {
    peer_ip_.assign(ip);
    peer_port_ = port;
  }

  void add_header(std::string_view field, std::string_view value);
  [[nodiscard]] std::string header(std::string_view field) const;
  [[nodiscard]] const std::map<std::string, std::string>& headers() const noexcept { return headers_; }

 private:
  HttpMethod method_{HttpMethod::invalid};
  HttpVersion version_{HttpVersion::unknown};
  std::string path_;
  std::string query_;
  oklib::Timestamp receive_time_;
  std::string peer_ip_;
  uint16_t peer_port_{0};
  std::map<std::string, std::string> headers_;
};

}  // namespace oklib::http
