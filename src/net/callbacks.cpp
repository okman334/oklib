#include "oklib/net/callbacks.h"

#include "oklib/net/buffer.h"

namespace oklib::net {

void default_connection_callback(const TcpConnectionPtr&) {}

void default_message_callback(const TcpConnectionPtr&, Buffer* buffer, oklib::Timestamp) {
  buffer->retrieve_all();
}

}  // namespace oklib::net
