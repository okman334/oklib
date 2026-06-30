#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "oklib/net/inet_address.h"

namespace oklib::net {

[[nodiscard]] std::vector<InetAddress> resolve_tcp_addresses(std::string_view host,
                                                             std::uint16_t port);

}  // namespace oklib::net
