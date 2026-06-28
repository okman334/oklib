#include "socks5_protocol.h"

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::vector<std::uint8_t> bytes(std::initializer_list<std::uint8_t> values) {
  return std::vector<std::uint8_t>(values);
}

}  // namespace

int main() {
  using namespace oklib::examples::socks5;

  AuthConfig no_auth;
  auto method = choose_auth_method(bytes({0x00, 0x02}), no_auth);
  require(method == AuthMethod::no_auth,
          "no-auth server selects no-auth when offered");

  AuthConfig password_auth;
  password_auth.username = "alice";
  password_auth.password = "secret";
  method = choose_auth_method(bytes({0x00, 0x02}), password_auth);
  require(method == AuthMethod::username_password,
          "password server selects username/password when offered");
  method = choose_auth_method(bytes({0x00}), password_auth);
  require(method == AuthMethod::no_acceptable_methods,
          "password server rejects clients that do not offer username/password");

  auto auth_ok = parse_user_password_auth(
      bytes({0x01, 0x05, 'a', 'l', 'i', 'c', 'e', 0x06, 's', 'e', 'c', 'r',
             'e', 't'}),
      password_auth);
  require(auth_ok.ok, "valid username/password auth succeeds");
  require(auth_ok.consumed == 14, "valid auth consumes all auth bytes");

  auto auth_bad = parse_user_password_auth(
      bytes({0x01, 0x05, 'a', 'l', 'i', 'c', 'e', 0x05, 'w', 'r', 'o', 'n',
             'g'}),
      password_auth);
  require(!auth_bad.ok, "wrong password fails");
  require(auth_bad.consumed == 13,
          "failed auth consumes the complete auth packet");

  auto ipv4 = parse_connect_request(
      bytes({0x05, 0x01, 0x00, 0x01, 127, 0, 0, 1, 0x1F, 0x90}));
  require(ipv4.status == ParseStatus::complete, "IPv4 connect request parses");
  require(ipv4.target.host == "127.0.0.1", "IPv4 host is formatted");
  require(ipv4.target.port == 8080, "IPv4 port is parsed");
  require(ipv4.consumed == 10, "IPv4 request consumes exact request length");

  auto domain = parse_connect_request(
      bytes({0x05, 0x01, 0x00, 0x03, 11, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
             '.', 'c', 'o', 'm', 0x00, 0x50}));
  require(domain.status == ParseStatus::complete,
          "domain connect request parses");
  require(domain.target.host == "example.com", "domain host is parsed");
  require(domain.target.port == 80, "domain port is parsed");
  require(domain.consumed == 18,
          "domain request consumes exact request length");

  auto short_request =
      parse_connect_request(bytes({0x05, 0x01, 0x00, 0x03, 11, 'e'}));
  require(short_request.status == ParseStatus::need_more,
          "short domain request waits for more bytes");

  auto ipv6 = parse_connect_request(
      bytes({0x05, 0x01, 0x00, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
             0, 1, 0, 80}));
  require(ipv6.status == ParseStatus::error, "IPv6 request is rejected");
  require(ipv6.reply == ReplyCode::addr_type_not_supported,
          "IPv6 rejection maps to addr type reply");

  auto success = build_reply(ReplyCode::success);
  require(success == bytes({0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0}),
          "success reply uses IPv4 wildcard bind address");

  return EXIT_SUCCESS;
}
