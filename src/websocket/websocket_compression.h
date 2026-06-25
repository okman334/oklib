#pragma once

#include <string>
#include <string_view>

namespace oklib::websocket {

bool websocket_deflate(std::string_view input, std::string* output);
bool websocket_inflate(std::string_view input, std::string* output);

}  // namespace oklib::websocket
