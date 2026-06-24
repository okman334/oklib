#include "oklib/http/http_response_writer.h"

#include <charconv>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "oklib/net/buffer.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/tcp_connection.h"

namespace oklib::http {
namespace {

struct PendingResponse {
  std::vector<std::string> chunks;
  bool close_connection{false};
  bool finished{false};
};

}  // namespace

struct HttpResponseWriter::State {
  explicit State(oklib::net::TcpConnectionPtr conn) : connection(std::move(conn)) {}

  std::weak_ptr<oklib::net::TcpConnection> connection;
  std::mutex mutex;
  std::map<std::size_t, PendingResponse> pending;
  std::size_t next_to_send{0};
  bool closed{false};
};

HttpResponseWriter::StatePtr HttpResponseWriter::make_state(
    const oklib::net::TcpConnectionPtr& connection) {
  return std::make_shared<State>(connection);
}

HttpResponseWriter::HttpResponseWriter(StatePtr state,
                                       std::size_t sequence,
                                       bool close_connection,
                                       bool include_body)
    : state_(std::move(state)),
      sequence_(sequence),
      close_connection_(close_connection),
      include_body_(include_body),
      write_state_(std::make_shared<std::atomic<int>>(0)) {}

bool HttpResponseWriter::send(HttpResponse response) const {
  if (!state_ || !write_state_) {
    return false;
  }

  int expected = 0;
  if (!write_state_->compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
    return false;
  }

  if (close_connection_) {
    response.set_close_connection(true);
  }

  oklib::net::Buffer output;
  response.append_to_buffer(&output, include_body_);
  return enqueue(output.retrieve_all_as_string(), true, response.close_connection());
}

bool HttpResponseWriter::start_chunked(HttpResponse response) const {
  if (!state_ || !write_state_) {
    return false;
  }

  int expected = 0;
  if (!write_state_->compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
    return false;
  }

  if (close_connection_) {
    response.set_close_connection(true);
  }

  oklib::net::Buffer output;
  response.append_headers_to_buffer(&output, true);
  if (!include_body_) {
    return enqueue(output.retrieve_all_as_string(), true, response.close_connection());
  }
  return enqueue(output.retrieve_all_as_string(), false, response.close_connection());
}

bool HttpResponseWriter::write_chunk(std::string_view data) const {
  if (!state_ || !write_state_ || write_state_->load(std::memory_order_acquire) != 1) {
    return false;
  }
  if (data.empty()) {
    return true;
  }
  return enqueue(encode_chunk(data), false, close_connection_);
}

bool HttpResponseWriter::finish() const {
  if (!state_ || !write_state_) {
    return false;
  }

  int expected = 1;
  if (!write_state_->compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
    return false;
  }
  return enqueue("0\r\n\r\n", true, close_connection_);
}

bool HttpResponseWriter::enqueue(std::string bytes, bool finished, bool close_connection) const {
  {
    std::lock_guard lock(state_->mutex);
    if (state_->closed) {
      return false;
    }

    auto& pending = state_->pending[sequence_];
    pending.chunks.push_back(std::move(bytes));
    pending.close_connection = pending.close_connection || close_connection;
    pending.finished = pending.finished || finished;
  }

  auto connection = state_->connection.lock();
  if (!connection) {
    return false;
  }
  auto state = state_;
  connection->loop()->queue_in_loop([state = std::move(state)] {
    HttpResponseWriter::flush_ready_responses(state);
  });
  return true;
}

std::string HttpResponseWriter::encode_chunk(std::string_view data) {
  char size[sizeof(std::size_t) * 2 + 1];
  auto [ptr, ec] = std::to_chars(size, size + sizeof(size), data.size(), 16);
  (void)ec;
  std::string chunk;
  chunk.reserve(static_cast<std::size_t>(ptr - size) + data.size() + 4);
  chunk.append(size, static_cast<std::size_t>(ptr - size));
  chunk.append("\r\n");
  chunk.append(data);
  chunk.append("\r\n");
  return chunk;
}

void HttpResponseWriter::flush_ready_responses(const StatePtr& state) {
  auto connection = state->connection.lock();
  if (!connection) {
    return;
  }

  std::vector<PendingResponse> ready;
  bool close_after_ready{false};
  {
    std::lock_guard lock(state->mutex);
    if (state->closed) {
      return;
    }

    for (;;) {
      auto it = state->pending.find(state->next_to_send);
      if (it == state->pending.end()) {
        break;
      }

      if (!it->second.chunks.empty()) {
        PendingResponse response;
        response.chunks.swap(it->second.chunks);
        response.close_connection = it->second.close_connection;
        response.finished = it->second.finished;
        ready.push_back(std::move(response));
      }

      if (!it->second.finished) {
        break;
      }

      const bool close_connection = it->second.close_connection;
      state->pending.erase(it);
      ++state->next_to_send;

      if (close_connection) {
        state->closed = true;
        close_after_ready = true;
        break;
      }
    }
  }

  for (const auto& response : ready) {
    for (const auto& chunk : response.chunks) {
      connection->send(chunk);
    }
  }
  if (close_after_ready) {
    connection->shutdown();
  }
}

}  // namespace oklib::http
