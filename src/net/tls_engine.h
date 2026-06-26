#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "oklib/net/tls_options.h"

#if OKLIB_ENABLE_TLS
#include <openssl/ssl.h>
#endif

namespace oklib::net {

#if OKLIB_ENABLE_TLS

class TlsEngine {
 public:
  ~TlsEngine();

  static std::unique_ptr<TlsEngine> create_client(const TlsClientOptions& options,
                                                 std::string_view host,
                                                 std::string* error);
  static std::unique_ptr<TlsEngine> create_server(const TlsServerOptions& options,
                                                 std::string* error);

  [[nodiscard]] bool handshake_complete() const noexcept { return handshake_complete_; }

  bool receive_encrypted(std::string_view data, std::string* error);
  bool do_handshake(std::string* encrypted_out, std::string* error);
  bool read_decrypted(std::string* plain_out, std::string* encrypted_out, std::string* error);
  bool encrypt(std::string_view plain, std::string* encrypted_out, std::string* error);

 private:
  TlsEngine(SSL_CTX* context, SSL* ssl);

  bool drain_encrypted(std::string* encrypted_out, std::string* error);
  bool openssl_ok_or_want(int result, std::string* error);

  SSL_CTX* context_{nullptr};
  SSL* ssl_{nullptr};
  BIO* read_bio_{nullptr};
  BIO* write_bio_{nullptr};
  bool handshake_complete_{false};
};

#endif

}  // namespace oklib::net
