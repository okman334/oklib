#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "oklib/base/timestamp.h"

namespace oklib::net {

class Buffer;
class TcpConnection;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, std::size_t)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*, oklib::Timestamp)>;
using ConnectFailedCallback = std::function<void()>;

void default_connection_callback(const TcpConnectionPtr& connection);
void default_message_callback(const TcpConnectionPtr& connection, Buffer* buffer, oklib::Timestamp receive_time);

}  // namespace oklib::net
