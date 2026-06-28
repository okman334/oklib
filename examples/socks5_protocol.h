#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oklib::examples::socks5 {

inline constexpr std::uint8_t kVersion = 0x05;
inline constexpr std::uint8_t kAuthVersion = 0x01;

enum class AuthMethod : std::uint8_t {
  no_auth = 0x00,
  username_password = 0x02,
  no_acceptable_methods = 0xff,
};

enum class ReplyCode : std::uint8_t {
  success = 0x00,
  general_failure = 0x01,
  command_not_supported = 0x07,
  addr_type_not_supported = 0x08,
};

enum class ParseStatus {
  need_more,
  complete,
  error,
};

struct AuthConfig {
  std::string username;
  std::string password;

  [[nodiscard]] bool enabled() const noexcept {
    return !username.empty() || !password.empty();
  }
};

struct AuthParseResult {
  ParseStatus status{ParseStatus::need_more};
  bool ok{false};
  std::size_t consumed{0};
};

struct TargetAddress {
  std::string host;
  std::uint16_t port{0};
};

struct RequestParseResult {
  ParseStatus status{ParseStatus::need_more};
  ReplyCode reply{ReplyCode::success};
  TargetAddress target;
  std::size_t consumed{0};
};

inline bool contains_method(std::string_view methods, AuthMethod method) {
  const auto value = static_cast<char>(static_cast<std::uint8_t>(method));
  return std::find(methods.begin(), methods.end(), value) != methods.end();
}

inline AuthMethod choose_auth_method(std::string_view methods,
                                     const AuthConfig& auth) {
  if (auth.enabled()) {
    return contains_method(methods, AuthMethod::username_password)
               ? AuthMethod::username_password
               : AuthMethod::no_acceptable_methods;
  }
  return contains_method(methods, AuthMethod::no_auth)
             ? AuthMethod::no_auth
             : AuthMethod::no_acceptable_methods;
}

inline AuthMethod choose_auth_method(const std::vector<std::uint8_t>& methods,
                                     const AuthConfig& auth) {
  return choose_auth_method(
      std::string_view(reinterpret_cast<const char*>(methods.data()),
                       methods.size()),
      auth);
}

inline std::vector<std::uint8_t> build_method_selection(AuthMethod method) {
  return {kVersion, static_cast<std::uint8_t>(method)};
}

inline std::vector<std::uint8_t> build_auth_status(bool ok) {
  return {kAuthVersion, static_cast<std::uint8_t>(ok ? 0x00 : 0x01)};
}

inline std::vector<std::uint8_t> build_reply(ReplyCode reply) {
  return {kVersion, static_cast<std::uint8_t>(reply), 0x00, 0x01, 0,
          0,        0,                                0,    0,    0};
}

inline AuthParseResult parse_user_password_auth(std::string_view data,
                                                const AuthConfig& auth) {
  if (data.size() < 2) {
    return {};
  }
  const auto version = static_cast<std::uint8_t>(data[0]);
  const auto username_len = static_cast<std::uint8_t>(data[1]);
  if (version != kAuthVersion) {
    return AuthParseResult{ParseStatus::error, false, 2};
  }
  const std::size_t password_len_index = 2 + username_len;
  if (data.size() <= password_len_index) {
    return {};
  }
  const auto password_len =
      static_cast<std::uint8_t>(data[password_len_index]);
  const std::size_t total = password_len_index + 1 + password_len;
  if (data.size() < total) {
    return {};
  }
  const std::string_view username(data.data() + 2, username_len);
  const std::string_view password(data.data() + password_len_index + 1,
                                  password_len);
  const bool ok = username == auth.username && password == auth.password;
  return AuthParseResult{ParseStatus::complete, ok, total};
}

inline AuthParseResult parse_user_password_auth(
    const std::vector<std::uint8_t>& data, const AuthConfig& auth) {
  return parse_user_password_auth(
      std::string_view(reinterpret_cast<const char*>(data.data()), data.size()),
      auth);
}

inline std::uint16_t read_port(std::string_view data, std::size_t offset) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[offset]))
       << 8) |
      static_cast<std::uint8_t>(data[offset + 1]));
}

inline RequestParseResult parse_connect_request(std::string_view data) {
  if (data.size() < 4) {
    return {};
  }
  const auto version = static_cast<std::uint8_t>(data[0]);
  const auto command = static_cast<std::uint8_t>(data[1]);
  const auto reserved = static_cast<std::uint8_t>(data[2]);
  const auto address_type = static_cast<std::uint8_t>(data[3]);
  if (version != kVersion) {
    return RequestParseResult{ParseStatus::error, ReplyCode::general_failure,
                              {}, 1};
  }
  if (command != 0x01) {
    return RequestParseResult{ParseStatus::error,
                              ReplyCode::command_not_supported, {}, 2};
  }
  if (reserved != 0x00) {
    return RequestParseResult{ParseStatus::error, ReplyCode::general_failure,
                              {}, 3};
  }
  if (address_type == 0x01) {
    if (data.size() < 10) {
      return {};
    }
    std::string host = std::to_string(static_cast<std::uint8_t>(data[4])) +
                       "." +
                       std::to_string(static_cast<std::uint8_t>(data[5])) +
                       "." +
                       std::to_string(static_cast<std::uint8_t>(data[6])) +
                       "." +
                       std::to_string(static_cast<std::uint8_t>(data[7]));
    return RequestParseResult{ParseStatus::complete,
                              ReplyCode::success,
                              TargetAddress{std::move(host),
                                            read_port(data, 8)},
                              10};
  }
  if (address_type == 0x03) {
    if (data.size() < 5) {
      return {};
    }
    const auto domain_len = static_cast<std::uint8_t>(data[4]);
    if (domain_len == 0) {
      return RequestParseResult{ParseStatus::error, ReplyCode::general_failure,
                                {}, 5};
    }
    const std::size_t total = 5 + domain_len + 2;
    if (data.size() < total) {
      return {};
    }
    std::string host(data.data() + 5, domain_len);
    return RequestParseResult{ParseStatus::complete,
                              ReplyCode::success,
                              TargetAddress{std::move(host),
                                            read_port(data, 5 + domain_len)},
                              total};
  }
  return RequestParseResult{ParseStatus::error,
                            ReplyCode::addr_type_not_supported, {}, 4};
}

inline RequestParseResult parse_connect_request(
    const std::vector<std::uint8_t>& data) {
  return parse_connect_request(
      std::string_view(reinterpret_cast<const char*>(data.data()),
                       data.size()));
}

}  // namespace oklib::examples::socks5
