#include "tls_engine.h"

#if OKLIB_ENABLE_TLS

#include <algorithm>
#include <array>
#include <mutex>

#include <openssl/err.h>

namespace oklib::net {
namespace {

void initialize_openssl_once() {
  static std::once_flag once;
  std::call_once(once, [] {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
  });
}

std::string last_openssl_error() {
  unsigned long code = ERR_get_error();
  if (code == 0) {
    return "OpenSSL error";
  }
  std::array<char, 256> buffer{};
  ERR_error_string_n(code, buffer.data(), buffer.size());
  return std::string(buffer.data());
}

bool load_verify_paths(SSL_CTX* context,
                       const std::string& ca_file,
                       const std::string& ca_path,
                       std::string* error) {
  const char* file = ca_file.empty() ? nullptr : ca_file.c_str();
  const char* path = ca_path.empty() ? nullptr : ca_path.c_str();
  if (file == nullptr && path == nullptr) {
    if (SSL_CTX_set_default_verify_paths(context) == 1) {
      return true;
    }
    if (error) {
      *error = last_openssl_error();
    }
    return false;
  }
  if (SSL_CTX_load_verify_locations(context, file, path) == 1) {
    return true;
  }
  if (error) {
    *error = last_openssl_error();
  }
  return false;
}

}  // namespace

TlsEngine::TlsEngine(SSL_CTX* context, SSL* ssl)
    : context_(context),
      ssl_(ssl) {}

TlsEngine::~TlsEngine() {
  if (ssl_ != nullptr) {
    SSL_free(ssl_);
  }
  if (context_ != nullptr) {
    SSL_CTX_free(context_);
  }
}

std::unique_ptr<TlsEngine> TlsEngine::create_client(const TlsClientOptions& options,
                                                   std::string_view host,
                                                   std::string* error) {
  initialize_openssl_once();
  SSL_CTX* context = SSL_CTX_new(TLS_client_method());
  if (context == nullptr) {
    if (error) {
      *error = last_openssl_error();
    }
    return nullptr;
  }

  if (options.verify_peer) {
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
    if (!load_verify_paths(context, options.ca_file, options.ca_path, error)) {
      SSL_CTX_free(context);
      return nullptr;
    }
  } else {
    SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
  }

  SSL* ssl = SSL_new(context);
  if (ssl == nullptr) {
    if (error) {
      *error = last_openssl_error();
    }
    SSL_CTX_free(context);
    return nullptr;
  }

  std::string server_name = options.server_name.empty() ? std::string(host) : options.server_name;
  if (!server_name.empty()) {
    SSL_set_tlsext_host_name(ssl, server_name.c_str());
    if (options.verify_peer) {
      SSL_set1_host(ssl, server_name.c_str());
    }
  }

  BIO* read_bio = BIO_new(BIO_s_mem());
  BIO* write_bio = BIO_new(BIO_s_mem());
  if (read_bio == nullptr || write_bio == nullptr) {
    if (error) {
      *error = last_openssl_error();
    }
    BIO_free(read_bio);
    BIO_free(write_bio);
    SSL_free(ssl);
    SSL_CTX_free(context);
    return nullptr;
  }
  BIO_set_mem_eof_return(read_bio, -1);
  BIO_set_mem_eof_return(write_bio, -1);
  SSL_set_bio(ssl, read_bio, write_bio);
  SSL_set_connect_state(ssl);

  auto engine = std::unique_ptr<TlsEngine>(new TlsEngine(context, ssl));
  engine->read_bio_ = read_bio;
  engine->write_bio_ = write_bio;
  return engine;
}

std::unique_ptr<TlsEngine> TlsEngine::create_server(const TlsServerOptions& options,
                                                   std::string* error) {
  initialize_openssl_once();
  SSL_CTX* context = SSL_CTX_new(TLS_server_method());
  if (context == nullptr) {
    if (error) {
      *error = last_openssl_error();
    }
    return nullptr;
  }

  if (SSL_CTX_use_certificate_file(context, options.cert_file.c_str(), SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_use_PrivateKey_file(context, options.key_file.c_str(), SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_check_private_key(context) != 1) {
    if (error) {
      *error = last_openssl_error();
    }
    SSL_CTX_free(context);
    return nullptr;
  }

  if (options.verify_peer) {
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    if (!load_verify_paths(context, options.ca_file, options.ca_path, error)) {
      SSL_CTX_free(context);
      return nullptr;
    }
  }

  SSL* ssl = SSL_new(context);
  if (ssl == nullptr) {
    if (error) {
      *error = last_openssl_error();
    }
    SSL_CTX_free(context);
    return nullptr;
  }

  BIO* read_bio = BIO_new(BIO_s_mem());
  BIO* write_bio = BIO_new(BIO_s_mem());
  if (read_bio == nullptr || write_bio == nullptr) {
    if (error) {
      *error = last_openssl_error();
    }
    BIO_free(read_bio);
    BIO_free(write_bio);
    SSL_free(ssl);
    SSL_CTX_free(context);
    return nullptr;
  }
  BIO_set_mem_eof_return(read_bio, -1);
  BIO_set_mem_eof_return(write_bio, -1);
  SSL_set_bio(ssl, read_bio, write_bio);
  SSL_set_accept_state(ssl);

  auto engine = std::unique_ptr<TlsEngine>(new TlsEngine(context, ssl));
  engine->read_bio_ = read_bio;
  engine->write_bio_ = write_bio;
  return engine;
}

bool TlsEngine::receive_encrypted(std::string_view data, std::string* error) {
  while (!data.empty()) {
    const int written = BIO_write(read_bio_, data.data(), static_cast<int>(data.size()));
    if (written <= 0) {
      if (error) {
        *error = last_openssl_error();
      }
      return false;
    }
    data.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

bool TlsEngine::do_handshake(std::string* encrypted_out, std::string* error) {
  if (handshake_complete_) {
    return drain_encrypted(encrypted_out, error);
  }

  const int result = SSL_do_handshake(ssl_);
  if (result == 1) {
    handshake_complete_ = true;
    return drain_encrypted(encrypted_out, error);
  }
  if (!openssl_ok_or_want(result, error)) {
    return false;
  }
  return drain_encrypted(encrypted_out, error);
}

bool TlsEngine::read_decrypted(std::string* plain_out, std::string* encrypted_out, std::string* error) {
  if (!do_handshake(encrypted_out, error)) {
    return false;
  }
  if (!handshake_complete_) {
    return true;
  }

  std::array<char, 16 * 1024> buffer{};
  for (;;) {
    std::size_t read = 0;
    const int result = SSL_read_ex(ssl_, buffer.data(), buffer.size(), &read);
    if (result == 1) {
      plain_out->append(buffer.data(), read);
      continue;
    }
    const int ssl_error = SSL_get_error(ssl_, result);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE ||
        ssl_error == SSL_ERROR_ZERO_RETURN) {
      return drain_encrypted(encrypted_out, error);
    }
    if (error) {
      *error = last_openssl_error();
    }
    return false;
  }
}

bool TlsEngine::encrypt(std::string_view plain, std::string* encrypted_out, std::string* error) {
  if (!handshake_complete_) {
    if (error) {
      *error = "TLS handshake is not complete";
    }
    return false;
  }

  while (!plain.empty()) {
    std::size_t written = 0;
    const int result = SSL_write_ex(ssl_, plain.data(), plain.size(), &written);
    if (result != 1) {
      const int ssl_error = SSL_get_error(ssl_, result);
      if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        return drain_encrypted(encrypted_out, error);
      }
      if (error) {
        *error = last_openssl_error();
      }
      return false;
    }
    plain.remove_prefix(written);
  }
  return drain_encrypted(encrypted_out, error);
}

bool TlsEngine::drain_encrypted(std::string* encrypted_out, std::string* error) {
  std::array<char, 16 * 1024> buffer{};
  for (;;) {
    const int pending = BIO_pending(write_bio_);
    if (pending <= 0) {
      return true;
    }
    const int read = BIO_read(write_bio_, buffer.data(), std::min<int>(pending, buffer.size()));
    if (read <= 0) {
      if (error) {
        *error = last_openssl_error();
      }
      return false;
    }
    encrypted_out->append(buffer.data(), static_cast<std::size_t>(read));
  }
}

bool TlsEngine::openssl_ok_or_want(int result, std::string* error) {
  const int ssl_error = SSL_get_error(ssl_, result);
  if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
    return true;
  }
  if (error) {
    *error = last_openssl_error();
  }
  return false;
}

}  // namespace oklib::net

#endif
