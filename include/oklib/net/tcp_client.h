#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "oklib/base/noncopyable.h"
#include "oklib/net/callbacks.h"
#include "oklib/net/connector.h"
#include "oklib/net/inet_address.h"
#include "oklib/net/tls_options.h"

namespace oklib::net {

class EventLoop;

class TcpClient : private oklib::Noncopyable {
 public:
  TcpClient(EventLoop* loop, const InetAddress& server_address, std::string name);
  TcpClient(EventLoop* loop, std::vector<InetAddress> server_addresses, std::string name);
  ~TcpClient();

  void connect();
  void disconnect();
  void stop();
  void enable_retry() { connector_->set_retry(true); }

  void set_connection_callback(ConnectionCallback callback) { connection_callback_ = std::move(callback); }
  void set_message_callback(MessageCallback callback) { message_callback_ = std::move(callback); }
  void set_write_complete_callback(WriteCompleteCallback callback) { write_complete_callback_ = std::move(callback); }
  void set_tls_options(TlsClientOptions options) { tls_options_ = std::move(options); }

  [[nodiscard]] TcpConnectionPtr connection() const;

 private:
  void new_connection(int sockfd);
  void remove_connection(const TcpConnectionPtr& connection);

  EventLoop* loop_;
  std::shared_ptr<Connector> connector_;
  InetAddress server_address_;
  std::string name_;
  ConnectionCallback connection_callback_{default_connection_callback};
  MessageCallback message_callback_{default_message_callback};
  WriteCompleteCallback write_complete_callback_;
  TlsClientOptions tls_options_;
  mutable std::mutex mutex_;
  TcpConnectionPtr connection_;
  bool retry_{false};
  bool connect_{true};
  int next_connection_id_{1};
};

}  // namespace oklib::net
