#include "oklib/http/http_response_writer.h"

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
  std::string bytes;
  bool close_connection{false};
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
      sent_(std::make_shared<std::atomic_bool>(false)) {}

bool HttpResponseWriter::send(HttpResponse response) const {
  if (!state_ || !sent_) {
    return false;
  }

  bool expected = false;
  if (!sent_->compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return false;
  }

  if (close_connection_) {
    response.set_close_connection(true);
  }

  oklib::net::Buffer output;
  response.append_to_buffer(&output, include_body_);
  PendingResponse pending{output.retrieve_all_as_string(), response.close_connection()};

  {
    std::lock_guard lock(state_->mutex);
    if (state_->closed) {
      return false;
    }
    state_->pending.emplace(sequence_, std::move(pending));
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

void HttpResponseWriter::flush_ready_responses(const StatePtr& state) {
  auto connection = state->connection.lock();
  if (!connection) {
    return;
  }

  std::vector<PendingResponse> ready;
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

      ready.push_back(std::move(it->second));
      state->pending.erase(it);
      ++state->next_to_send;

      if (ready.back().close_connection) {
        state->closed = true;
        break;
      }
    }
  }

  for (const auto& response : ready) {
    connection->send(response.bytes);
    if (response.close_connection) {
      connection->shutdown();
      break;
    }
  }
}

}  // namespace oklib::http
