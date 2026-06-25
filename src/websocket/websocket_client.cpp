#include "oklib/websocket/websocket_client.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "oklib/http/http_headers.h"
#include "oklib/net/buffer.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"
#include "oklib/net/tcp_connection.h"
#include "oklib/websocket/websocket_handshake.h"
#include "websocket_compression.h"
#if OKLIB_ENABLE_TLS
#include "../http/tls_engine.h"
#endif

namespace oklib::websocket {
namespace {

#if OKLIB_ENABLE_WEBSOCKET_COMPRESSION
constexpr bool k_compression_available = true;
#else
constexpr bool k_compression_available = false;
#endif

std::string lower(std::string_view value) {
  std::string result(value);
  for (char& ch : result) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return result;
}

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
        std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

std::string_view trim_ows(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

bool header_has_token(std::string_view value, std::string_view token) {
  while (!value.empty()) {
    const auto comma = value.find(',');
    const auto part = trim_ows(comma == std::string_view::npos ? value : value.substr(0, comma));
    if (equals_ignore_case(part, token)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      return false;
    }
    value.remove_prefix(comma + 1);
  }
  return false;
}

bool parse_port(std::string_view value, std::uint16_t* port) {
  if (value.empty()) {
    return false;
  }
  unsigned parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed == 0 || parsed > 65535) {
    return false;
  }
  *port = static_cast<std::uint16_t>(parsed);
  return true;
}

std::string strip_fragment(std::string_view target) {
  const auto fragment = target.find('#');
  if (fragment != std::string_view::npos) {
    target = target.substr(0, fragment);
  }
  return std::string(target.empty() ? std::string_view("/") : target);
}

bool is_reserved_request_header(std::string_view field) {
  return equals_ignore_case(field, "Host") ||
         equals_ignore_case(field, "Upgrade") ||
         equals_ignore_case(field, "Connection") ||
         equals_ignore_case(field, "Sec-WebSocket-Key") ||
         equals_ignore_case(field, "Sec-WebSocket-Version") ||
         equals_ignore_case(field, "Sec-WebSocket-Extensions");
}

std::optional<oklib::net::InetAddress> resolve_ipv4(std::string_view host, std::uint16_t port) {
  std::string host_string(host);
  if (lower(host_string) == "localhost") {
    host_string = "127.0.0.1";
  }

  try {
    return oklib::net::InetAddress(host_string, port);
  } catch (const std::invalid_argument&) {
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host_string.c_str(), service.c_str(), &hints, &result) != 0) {
    return std::nullopt;
  }
  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(result, ::freeaddrinfo);
  for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
    if (ai->ai_family == AF_INET && ai->ai_addrlen >= sizeof(sockaddr_in)) {
      return oklib::net::InetAddress(*reinterpret_cast<sockaddr_in*>(ai->ai_addr));
    }
  }
  return std::nullopt;
}

bool response_accepts_permessage_deflate(const oklib::http::HttpResponseMessage& response) {
  for (const auto& value : response.headers.values("Sec-WebSocket-Extensions")) {
    if (lower(value).find("permessage-deflate") != std::string::npos) {
      return true;
    }
  }
  return false;
}

WebSocketCloseInfo close_info_from_payload(std::string_view payload, bool* ok) {
  *ok = true;
  WebSocketCloseInfo info;
  if (payload.empty()) {
    return info;
  }
  if (payload.size() == 1) {
    *ok = false;
    return info;
  }
  info.code = static_cast<std::uint16_t>(
      (static_cast<unsigned char>(payload[0]) << 8) |
      static_cast<unsigned char>(payload[1]));
  if (!valid_close_code(info.code)) {
    *ok = false;
    return info;
  }
  info.reason.assign(payload.substr(2));
  if (!info.reason.empty() && !valid_utf8(info.reason)) {
    *ok = false;
  }
  return info;
}

std::uint16_t close_code_for_error(WebSocketError error) {
  switch (error) {
    case WebSocketError::message_too_big:
      return 1009;
    case WebSocketError::invalid_utf8:
      return 1007;
    default:
      return 1002;
  }
}

struct ParsedEndpoint {
  oklib::net::InetAddress address;
  std::string host_header;
  std::string host_name;
  std::string target;
  bool secure{false};
};

std::optional<ParsedEndpoint> parse_endpoint(std::string_view url, int* error_code) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos || scheme_end == 0) {
    *error_code = static_cast<int>(WebSocketClientErrorCode::invalid_url);
    return std::nullopt;
  }
  const std::string scheme = lower(url.substr(0, scheme_end));
  const bool secure = scheme == "wss";
  if (scheme != "ws" && !secure) {
    *error_code = static_cast<int>(WebSocketClientErrorCode::unsupported_scheme);
    return std::nullopt;
  }

  std::string_view remainder = url.substr(scheme_end + 3);
  const auto target_start = remainder.find_first_of("/?#");
  std::string_view authority =
      target_start == std::string_view::npos ? remainder : remainder.substr(0, target_start);
  if (authority.empty() || authority.front() == '[') {
    *error_code = static_cast<int>(WebSocketClientErrorCode::unsupported_host);
    return std::nullopt;
  }

  std::string_view host = authority;
  std::uint16_t port = secure ? 443 : 80;
  const auto colon = authority.rfind(':');
  if (colon != std::string_view::npos) {
    if (colon == 0 || colon + 1 >= authority.size() ||
        !parse_port(authority.substr(colon + 1), &port)) {
      *error_code = static_cast<int>(WebSocketClientErrorCode::unsupported_host);
      return std::nullopt;
    }
    host = authority.substr(0, colon);
  }
  if (host.empty()) {
    *error_code = static_cast<int>(WebSocketClientErrorCode::unsupported_host);
    return std::nullopt;
  }

  std::string target = "/";
  if (target_start != std::string_view::npos) {
    const auto suffix = remainder.substr(target_start);
    if (!suffix.empty() && suffix.front() == '?') {
      target = "/" + strip_fragment(suffix);
    } else if (!suffix.empty() && suffix.front() == '#') {
      target = "/";
    } else {
      target = strip_fragment(suffix);
    }
  }

  auto address = resolve_ipv4(host, port);
  if (!address) {
    *error_code = static_cast<int>(WebSocketClientErrorCode::unsupported_host);
    return std::nullopt;
  }

  const bool default_port = (!secure && port == 80) || (secure && port == 443);
  std::string host_header(host);
  if (!default_port) {
    host_header += ":" + std::to_string(port);
  }

  return ParsedEndpoint{*address,
                        std::move(host_header),
                        std::string(host),
                        std::move(target),
                        secure};
}

}  // namespace

#if OKLIB_ENABLE_TLS
class WebSocketClient::TlsState {
 public:
  std::unique_ptr<oklib::http::TlsEngine> engine;
  oklib::net::Buffer plain_input;
};
#endif

WebSocketClient::WebSocketClient(oklib::net::EventLoop* loop,
                                 std::string name,
                                 WebSocketOptions options)
    : loop_(loop),
      name_(std::move(name)),
      options_(options),
      frame_parser_(WebSocketEndpointRole::client, options_),
      message_assembler_(options_) {}

WebSocketClient::~WebSocketClient() {
  stop();
}

int WebSocketClient::open(std::string url, oklib::http::HttpHeaders headers) {
  int error_code = 0;
  auto endpoint = parse_endpoint(url, &error_code);
  if (!endpoint) {
    report_error(error_code == static_cast<int>(WebSocketClientErrorCode::unsupported_scheme)
                     ? WebSocketError::unsupported_url
                     : WebSocketError::bad_handshake);
    return error_code;
  }

  if (endpoint->secure) {
#if OKLIB_ENABLE_TLS
    tls_options_.enabled = true;
    if (tls_options_.server_name.empty()) {
      tls_options_.server_name = endpoint->host_name;
    }
#else
    report_error(WebSocketError::tls_not_enabled);
    return static_cast<int>(WebSocketClientErrorCode::tls_not_enabled);
#endif
  } else {
    tls_options_.enabled = false;
  }

  stop();
  reset_connection_state();
  headers_ = std::move(headers);
  host_header_ = std::move(endpoint->host_header);
  host_name_ = std::move(endpoint->host_name);
  target_ = std::move(endpoint->target);
  websocket_key_ = websocket_generate_key();

  client_ = std::make_unique<oklib::net::TcpClient>(loop_, endpoint->address, name_);
  client_->set_connection_callback([this](const oklib::net::TcpConnectionPtr& connection) {
    on_connection(connection);
  });
  client_->set_message_callback([this](const oklib::net::TcpConnectionPtr& connection,
                                       oklib::net::Buffer* buffer,
                                       oklib::Timestamp receive_time) {
    on_message(connection, buffer, receive_time);
  });
  client_->connect();
  return static_cast<int>(WebSocketClientErrorCode::ok);
}

bool WebSocketClient::send_text(std::string_view message) {
  return channel_ && channel_->send_text(message);
}

bool WebSocketClient::send_binary(std::string_view message) {
  return channel_ && channel_->send_binary(message);
}

bool WebSocketClient::send_ping(std::string_view payload) {
  return channel_ && channel_->send_ping(payload);
}

bool WebSocketClient::close(std::uint16_t code, std::string_view reason) {
  return channel_ && channel_->close(code, reason);
}

void WebSocketClient::stop() {
  if (client_) {
    client_->disconnect();
    client_->stop();
  }
}

void WebSocketClient::on_connection(const oklib::net::TcpConnectionPtr& connection) {
  if (connection->connected()) {
    connection_ = connection;
    close_notified_ = false;
    if (tls_options_.enabled) {
      if (!start_tls_handshake()) {
        report_error(WebSocketError::connection_error);
        connection->force_close();
      }
      return;
    }
    send_handshake_request();
    return;
  }

  if (channel_) {
    WebSocketCloseInfo info;
    info.code = 1006;
    notify_close(std::move(info));
  }
  reset_connection_state();
}

void WebSocketClient::on_message(const oklib::net::TcpConnectionPtr&,
                                 oklib::net::Buffer* buffer,
                                 oklib::Timestamp) {
#if OKLIB_ENABLE_TLS
  if (tls_options_.enabled) {
    if (!tls_state_) {
      report_error(WebSocketError::connection_error);
      if (connection_) {
        connection_->force_close();
      }
      return;
    }
    if (!process_tls_input(buffer, &tls_state_->plain_input)) {
      report_error(WebSocketError::connection_error);
      if (connection_) {
        connection_->force_close();
      }
      return;
    }
    if (tls_state_->plain_input.readable_bytes() == 0) {
      return;
    }
    on_plain_message(&tls_state_->plain_input);
    return;
  }
#endif
  on_plain_message(buffer);
}

void WebSocketClient::send_handshake_request() {
  if (!connection_ || !connection_->connected()) {
    return;
  }
  std::string request;
  request.reserve(target_.size() + host_header_.size() + 256);
  request.append("GET ");
  request.append(target_.empty() ? "/" : target_);
  request.append(" HTTP/1.1\r\nHost: ");
  request.append(host_header_);
  request.append("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
  request.append(websocket_key_);
  request.append("\r\nSec-WebSocket-Version: 13\r\n");
  if (options_.enable_compression && k_compression_available) {
    request.append("Sec-WebSocket-Extensions: permessage-deflate; "
                   "client_no_context_takeover; server_no_context_takeover\r\n");
  }
  for (const auto& entry : headers_.entries()) {
    if (!is_reserved_request_header(entry.field)) {
      request.append(entry.field);
      request.append(": ");
      request.append(entry.value);
      request.append("\r\n");
    }
  }
  request.append("\r\n");
  connection_->send(request);
}

bool WebSocketClient::process_tls_input(oklib::net::Buffer* buffer,
                                        oklib::net::Buffer* plain) {
#if OKLIB_ENABLE_TLS
  if (!tls_state_ || !tls_state_->engine) {
    return false;
  }
  std::string error;
  const auto encrypted_input = buffer->retrieve_all_as_string();
  if (!tls_state_->engine->receive_encrypted(encrypted_input, &error)) {
    return false;
  }
  std::string encrypted_output;
  std::string plain_output;
  if (!tls_state_->engine->read_decrypted(&plain_output, &encrypted_output, &error)) {
    return false;
  }
  if (!encrypted_output.empty() && connection_) {
    connection_->send_raw(encrypted_output);
  }
  const bool became_ready = !tls_ready_ && tls_state_->engine->handshake_complete();
  tls_ready_ = tls_state_->engine->handshake_complete();
  if (became_ready) {
    send_handshake_request();
  }
  if (!plain_output.empty()) {
    plain->append(plain_output);
  }
  return true;
#else
  (void)buffer;
  (void)plain;
  return false;
#endif
}

bool WebSocketClient::start_tls_handshake() {
#if OKLIB_ENABLE_TLS
  tls_state_ = std::make_unique<TlsState>();
  std::string error;
  tls_state_->engine = oklib::http::TlsEngine::create_client(tls_options_, host_name_, &error);
  if (!tls_state_->engine) {
    return false;
  }
  std::weak_ptr<oklib::net::TcpConnection> weak = connection_;
  connection_->set_send_filter([this, weak](std::string_view plain) {
    auto connection = weak.lock();
    if (!connection || !tls_state_ || !tls_state_->engine) {
      return std::string{};
    }
    std::string encrypted;
    std::string error;
    if (!tls_state_->engine->encrypt(plain, &encrypted, &error)) {
      connection->force_close();
      return std::string{};
    }
    return encrypted;
  });

  std::string encrypted;
  if (!tls_state_->engine->do_handshake(&encrypted, &error)) {
    return false;
  }
  if (!encrypted.empty()) {
    connection_->send_raw(encrypted);
  }
  tls_ready_ = tls_state_->engine->handshake_complete();
  if (tls_ready_) {
    send_handshake_request();
  }
  return true;
#else
  return false;
#endif
}

void WebSocketClient::on_plain_message(oklib::net::Buffer* buffer) {
  if (handshake_complete_) {
    on_websocket_message(buffer);
    return;
  }

  const auto status = parser_.parse_response_head(buffer);
  if (status == oklib::http::HttpParseStatus::incomplete) {
    return;
  }
  if (status == oklib::http::HttpParseStatus::error) {
    parser_.reset();
    report_error(WebSocketError::bad_handshake);
    if (connection_) {
      connection_->force_close();
    }
    return;
  }

  const auto response = parser_.response();
  parser_.reset();
  const std::string expected_accept = websocket_accept_key(websocket_key_);
  if (response.status_code != 101 ||
      !header_has_token(response.headers.get("Connection"), "Upgrade") ||
      !equals_ignore_case(response.headers.get("Upgrade"), "websocket") ||
      response.headers.get("Sec-WebSocket-Accept") != expected_accept) {
    report_error(WebSocketError::bad_handshake);
    if (connection_) {
      connection_->force_close();
    }
    return;
  }

  const bool accepted_compression = response_accepts_permessage_deflate(response);
  if (accepted_compression && !(options_.enable_compression && k_compression_available)) {
    report_error(WebSocketError::protocol_error);
    if (connection_) {
      connection_->force_close();
    }
    return;
  }
  compression_enabled_ = accepted_compression;
  WebSocketOptions connection_options = options_;
  connection_options.enable_compression = compression_enabled_;
  frame_parser_ = WebSocketFrameParser(WebSocketEndpointRole::client, connection_options);
  message_assembler_ = WebSocketMessageAssembler(connection_options);
  channel_ = std::make_shared<WebSocketChannel>(
      connection_, WebSocketEndpointRole::client, connection_options, compression_enabled_);
  handshake_complete_ = true;
  if (open_callback_) {
    open_callback_(channel_);
  }
  if (buffer->readable_bytes() > 0) {
    on_websocket_message(buffer);
  }
}

void WebSocketClient::on_websocket_message(oklib::net::Buffer* buffer) {
  std::vector<WebSocketFrame> frames;
  const auto parse_status = frame_parser_.parse(buffer, &frames);
  if (parse_status == WebSocketParseStatus::error) {
    const auto error = frame_parser_.error();
    report_error(error);
    if (channel_) {
      channel_->close(close_code_for_error(error), "protocol error");
    }
    return;
  }

  for (const auto& frame : frames) {
    WebSocketMessage message;
    const auto message_status = message_assembler_.consume(frame, &message);
    if (message_status == WebSocketMessageStatus::error) {
      const auto error = message_assembler_.error();
      report_error(error);
      if (channel_) {
        channel_->close(close_code_for_error(error), "protocol error");
      }
      return;
    }

    if (message_status == WebSocketMessageStatus::control) {
      if (frame.opcode == WebSocketOpcode::ping) {
        if (channel_) {
          channel_->send_pong(frame.payload);
        }
        continue;
      }
      if (frame.opcode == WebSocketOpcode::pong) {
        continue;
      }
      if (frame.opcode == WebSocketOpcode::close) {
        bool ok = false;
        WebSocketCloseInfo info = close_info_from_payload(frame.payload, &ok);
        if (!ok) {
          report_error(WebSocketError::protocol_error);
          if (channel_) {
            channel_->close(1002, "bad close frame");
          }
          return;
        }
        if (channel_) {
          channel_->mark_close_received();
          if (!channel_->close_sent()) {
            const std::uint16_t code = info.code == 1005 ? 1000 : info.code;
            channel_->close(code, info.reason);
          }
        }
        notify_close(std::move(info));
        return;
      }
      continue;
    }

    if (message_status != WebSocketMessageStatus::complete) {
      continue;
    }

    if (message.compressed) {
      std::string inflated;
      if (!compression_enabled_ || !websocket_inflate(message.payload, &inflated)) {
        report_error(WebSocketError::compression_error);
        if (channel_) {
          channel_->close(1002, "compression error");
        }
        return;
      }
      message.payload = std::move(inflated);
      if (message.opcode == WebSocketOpcode::text && !valid_utf8(message.payload)) {
        report_error(WebSocketError::invalid_utf8);
        if (channel_) {
          channel_->close(1007, "invalid utf-8");
        }
        return;
      }
    }

    if (message_callback_) {
      message_callback_(channel_, message.payload, message.opcode);
    }
  }
}

void WebSocketClient::report_error(WebSocketError error) {
  if (error_callback_) {
    error_callback_(channel_, error);
  }
}

void WebSocketClient::notify_close(WebSocketCloseInfo info) {
  if (close_notified_) {
    return;
  }
  close_notified_ = true;
  if (close_callback_) {
    close_callback_(channel_, std::move(info));
  }
}

void WebSocketClient::reset_connection_state() {
  connection_.reset();
  channel_.reset();
  parser_.reset();
  frame_parser_ = WebSocketFrameParser(WebSocketEndpointRole::client, options_);
  message_assembler_ = WebSocketMessageAssembler(options_);
  handshake_complete_ = false;
  compression_enabled_ = false;
  tls_ready_ = false;
  close_notified_ = false;
#if OKLIB_ENABLE_TLS
  tls_state_.reset();
#endif
}

}  // namespace oklib::websocket
