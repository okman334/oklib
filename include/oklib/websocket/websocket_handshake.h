#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oklib::http {
class HttpRequest;
}

namespace oklib::websocket {

[[nodiscard]] std::string websocket_accept_key(std::string_view key);
[[nodiscard]] std::string websocket_generate_key();
[[nodiscard]] std::vector<std::string> websocket_split_tokens(std::string_view value);
[[nodiscard]] bool websocket_header_has_token(std::string_view value, std::string_view token);
[[nodiscard]] bool websocket_is_upgrade_request(const oklib::http::HttpRequest& request);
[[nodiscard]] std::vector<std::string> websocket_requested_subprotocols(
    const oklib::http::HttpRequest& request);
[[nodiscard]] std::optional<std::string> websocket_validate_client_key(std::string_view key);

}  // namespace oklib::websocket
