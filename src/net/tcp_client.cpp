#include "oklib/net/tcp_client.h"

#include <cstdio>

#include "oklib/net/event_loop.h"
#include "oklib/net/socket.h"
#include "oklib/net/tcp_connection.h"

namespace oklib::net {

TcpClient::TcpClient(EventLoop* loop, const InetAddress& server_address, std::string name)
    : loop_(loop),
      connector_(std::make_shared<Connector>(loop, server_address)),
      name_(std::move(name)) {
  connector_->set_new_connection_callback([this](int sockfd) { new_connection(sockfd); });
}

TcpClient::~TcpClient() {
  TcpConnectionPtr connection;
  {
    std::lock_guard lock(mutex_);
    connection = connection_;
  }
  if (connection) {
    connection->force_close();
  } else {
    connector_->stop();
  }
}

void TcpClient::connect() {
  connect_ = true;
  connector_->start();
}

void TcpClient::disconnect() {
  connect_ = false;
  auto conn = connection();
  if (conn) {
    conn->shutdown();
  }
}

void TcpClient::stop() {
  connect_ = false;
  connector_->stop();
}

TcpConnectionPtr TcpClient::connection() const {
  std::lock_guard lock(mutex_);
  return connection_;
}

void TcpClient::new_connection(int sockfd) {
  loop_->assert_in_loop_thread();
  Socket owned(sockfd);
  InetAddress peer_address = owned.peer_address();
  InetAddress local_address = owned.local_address();
  sockfd = owned.release();

  char buf[64];
  std::snprintf(buf, sizeof(buf), "-%s#%d", peer_address.to_ip_port().c_str(), next_connection_id_++);
  auto conn = std::make_shared<TcpConnection>(loop_, name_ + buf, sockfd, local_address, peer_address);
  conn->set_connection_callback(connection_callback_);
  conn->set_message_callback(message_callback_);
  conn->set_write_complete_callback(write_complete_callback_);
  conn->set_close_callback([this](const TcpConnectionPtr& connection) { remove_connection(connection); });
  {
    std::lock_guard lock(mutex_);
    connection_ = conn;
  }
  conn->connect_established();
}

void TcpClient::remove_connection(const TcpConnectionPtr& connection) {
  loop_->assert_in_loop_thread();
  {
    std::lock_guard lock(mutex_);
    if (connection_ == connection) {
      connection_.reset();
    }
  }
  loop_->queue_in_loop([connection] { connection->connect_destroyed(); });
  if (retry_ && connect_) {
    connector_->restart();
  }
}

}  // namespace oklib::net
