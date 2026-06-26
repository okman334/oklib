#pragma once

#include <string>

#ifndef OKLIB_ENABLE_TLS
#define OKLIB_ENABLE_TLS 0
#endif

namespace oklib::net {

struct TlsClientOptions {
  bool enabled{false};
  bool verify_peer{true};
  std::string server_name;
  std::string ca_file;
  std::string ca_path;
};

struct TlsServerOptions {
  bool enabled{false};
  std::string cert_file;
  std::string key_file;
  std::string ca_file;
  std::string ca_path;
  bool verify_peer{false};
};

}  // namespace oklib::net
