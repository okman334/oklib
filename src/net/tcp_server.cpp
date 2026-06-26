#include "oklib/net/tcp_server.h"

#include <cassert>
#include <cstdio>

#include "oklib/net/acceptor.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/event_loop_thread_pool.h"
#include "oklib/net/socket.h"
#include "oklib/net/tcp_connection.h"

namespace oklib::net {

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listen_address, std::string name, Option option)
    : loop_(loop),
      name_(std::move(name)),
      acceptor_(std::make_unique<Acceptor>(loop, listen_address, option == Option::reuse_port)),
      thread_pool_(std::make_shared<EventLoopThreadPool>(loop, name_)) {
  acceptor_->set_new_connection_callback(
      [this](int sockfd, const InetAddress& peer_address) { new_connection(sockfd, peer_address); });
}

TcpServer::~TcpServer() {
  for (auto& [_, connection] : connections_) {
    auto conn = connection;
    connection.reset();
    conn->loop()->run_in_loop([conn] { conn->connect_destroyed(); });
  }
}

uint16_t TcpServer::port() const {
  return listen_address().port();
}

InetAddress TcpServer::listen_address() const {
  return acceptor_->listen_address();
}

void TcpServer::set_thread_num(int num_threads) {
  assert(num_threads >= 0);
  thread_pool_->set_thread_num(num_threads);
}

void TcpServer::start() {
  bool expected = false;
  if (started_.compare_exchange_strong(expected, true)) {
    thread_pool_->start(thread_init_callback_);
    loop_->run_in_loop([this] {
      if (!acceptor_->listening()) {
        acceptor_->listen();
      }
    });
  }
}

void TcpServer::new_connection(int sockfd, const InetAddress& peer_address) {
  loop_->assert_in_loop_thread();
  EventLoop* io_loop = thread_pool_->get_next_loop();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "-%s#%d", listen_address().to_ip_port().c_str(), next_connection_id_++);
  std::string conn_name = name_ + buf;
  Socket owned(sockfd);
  InetAddress local_address = owned.local_address();
  sockfd = owned.release();

  auto connection =
      std::make_shared<TcpConnection>(io_loop, conn_name, sockfd, local_address, peer_address);
  connection->enable_server_tls(tls_options_);
  connections_[conn_name] = connection;
  connection->set_connection_callback(connection_callback_);
  connection->set_message_callback(message_callback_);
  connection->set_write_complete_callback(write_complete_callback_);
  connection->set_high_water_mark_callback(high_water_mark_callback_, high_water_mark_);
  connection->set_close_callback([this](const TcpConnectionPtr& conn) { remove_connection(conn); });
  io_loop->run_in_loop([connection] { connection->connect_established(); });
}

void TcpServer::remove_connection(const TcpConnectionPtr& connection) {
  loop_->run_in_loop([this, connection] { remove_connection_in_loop(connection); });
}

void TcpServer::remove_connection_in_loop(const TcpConnectionPtr& connection) {
  loop_->assert_in_loop_thread();
  connections_.erase(connection->name());
  EventLoop* io_loop = connection->loop();
  io_loop->queue_in_loop([connection] { connection->connect_destroyed(); });
}

}  // namespace oklib::net
