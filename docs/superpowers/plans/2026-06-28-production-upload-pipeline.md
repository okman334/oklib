# Production Upload Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the current upload demo into a production-oriented HTTP upload pipeline with bounded memory, worker-thread file writing, connection backpressure, cancellation cleanup, and low-copy data flow.

**Architecture:** Keep network reads on `EventLoop` threads and move blocking disk writes to a dedicated upload writer pool. Each upload owns a bounded queue of reusable byte blocks; high watermark pauses socket reads and low watermark resumes them. Completed files are written as `.part` files and atomically renamed after successful completion.

**Tech Stack:** C++20, existing `oklib::net` reactor/TCP classes, existing `oklib::http` streaming callbacks, standard library synchronization primitives, CMake/CTest.

---

## Design Constraints

- I/O threads must not call blocking filesystem writes for upload body data.
- Upload queues must be bounded by per-upload bytes and global bytes.
- The implementation must avoid whole-body buffering for raw uploads.
- Data passed from the HTTP parser to writer threads must have an owned lifetime; true zero-copy from socket buffer to async file writer is not safe because `Buffer` storage is reused after `retrieve`.
- Minimize deep copies by copying each chunk into reusable fixed-size upload blocks instead of allocating one `std::string` per chunk.
- Backpressure must happen by disabling socket read events, not by busy waiting in callbacks.
- Client disconnect, timeout, write error, and size-limit failures must close the session and remove `.part` files.
- Existing `/upload-file` behavior should remain compatible. New production behavior should be introduced under new reusable library APIs and then wired into examples.

## Proposed File Structure

- Modify `include/oklib/net/tcp_connection.h`
  - Add `pause_reading()`, `resume_reading()`, and `reading_paused()`.
- Modify `src/net/tcp_connection.cpp`
  - Implement read-control operations by queueing into the connection owner `EventLoop`.
- Modify `include/oklib/http/http_request_body_stream.h`
  - Add `pause_reading()`, `resume_reading()`, and cancellation callback support.
- Modify `src/http/http_request_body_stream.cpp`
  - Store read-control callbacks and cancellation callbacks in the stream state.
- Modify `src/http/http_server.cpp` and `src/http/http_context.cpp`
  - Pass read-control hooks from `TcpConnection` into streaming body handlers.
  - Notify stream cancellation when the connection closes before body completion.
- Create `include/oklib/http/upload_file_writer.h`
  - Public upload writer pool/session API.
- Create `src/http/upload_file_writer.cpp`
  - Dedicated file writer pool, block pool, bounded queues, `.part` file handling, rename, cleanup, metrics counters.
- Modify `CMakeLists.txt`
  - Add `src/http/upload_file_writer.cpp` to `oklib_http`.
- Modify `examples/http_demo_routes.h`
  - Replace experimental `/upload-file-worker` internals with `UploadFileWriter`.
- Add `tests/net/tcp_read_control_test.cpp`
  - Verify pause/resume reading prevents and resumes message callbacks.
- Add `tests/http/http_upload_writer_test.cpp`
  - Unit/integration coverage for writer session, queue watermarks, `.part` rename, cancellation cleanup.
- Update `tests/CMakeLists.txt`
  - Register new tests.

---

### Task 1: Add TCP Read Pause/Resume

**Files:**
- Modify: `include/oklib/net/tcp_connection.h`
- Modify: `src/net/tcp_connection.cpp`
- Test: `tests/net/tcp_read_control_test.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing TCP read-control test**

Add `tests/net/tcp_read_control_test.cpp`:

```cpp
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>
#include <oklib/net/tcp_client.h>
#include <oklib/net/tcp_server.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

void test_pause_and_resume_reading() {
  oklib::net::EventLoop loop;
  oklib::net::TcpConnectionPtr server_connection;
  int messages = 0;

  oklib::net::TcpServer server(&loop,
                               oklib::net::InetAddress::loopback(0),
                               "tcp-read-control-test");
  server.set_connection_callback([&](const oklib::net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
      server_connection = conn;
      conn->pause_reading();
    }
  });
  server.set_message_callback([&](const oklib::net::TcpConnectionPtr&,
                                  oklib::net::Buffer* buffer,
                                  oklib::Timestamp) {
    ++messages;
    buffer->retrieve_all();
  });
  server.start();

  oklib::net::TcpClient client(&loop,
                               server.listen_address(),
                               "tcp-read-control-client");
  client.set_connection_callback([&](const oklib::net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
      conn->send("before-resume");
      loop.run_after(std::chrono::milliseconds(80), [&] {
        require(messages == 0, "paused connection does not deliver reads");
        require(server_connection != nullptr, "server connection exists");
        server_connection->resume_reading();
      });
      loop.run_after(std::chrono::milliseconds(180), [&] {
        require(messages == 1, "resumed connection delivers pending read");
        client.disconnect();
        loop.quit();
      });
    }
  });
  client.connect();
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
}

}  // namespace

int main() {
  test_pause_and_resume_reading();
  return 0;
}
```

- [ ] **Step 2: Register the test**

Add to `tests/CMakeLists.txt` near the other net tests:

```cmake
add_executable(oklib_tcp_read_control_test net/tcp_read_control_test.cpp)
target_link_libraries(oklib_tcp_read_control_test PRIVATE oklib::net)
add_test(NAME oklib.tcp.read_control COMMAND oklib_tcp_read_control_test)
```

- [ ] **Step 3: Run the test to verify it fails**

Run:

```bash
cmake --build build --target oklib_tcp_read_control_test --parallel
```

Expected: compile failure because `TcpConnection::pause_reading()` and `resume_reading()` do not exist.

- [ ] **Step 4: Add public TCP read-control API**

Add to `include/oklib/net/tcp_connection.h`:

```cpp
void pause_reading();
void resume_reading();
[[nodiscard]] bool reading_paused() const noexcept { return reading_paused_; }
```

Add a private field:

```cpp
bool reading_paused_{false};
```

- [ ] **Step 5: Implement read-control API**

In `src/net/tcp_connection.cpp`, implement:

```cpp
void TcpConnection::pause_reading() {
  if (loop_->is_in_loop_thread()) {
    if (!reading_paused_) {
      reading_paused_ = true;
      channel_->disable_reading();
    }
    return;
  }
  auto self = shared_from_this();
  loop_->queue_in_loop([self] { self->pause_reading(); });
}

void TcpConnection::resume_reading() {
  if (loop_->is_in_loop_thread()) {
    if (reading_paused_) {
      reading_paused_ = false;
      channel_->enable_reading();
    }
    return;
  }
  auto self = shared_from_this();
  loop_->queue_in_loop([self] { self->resume_reading(); });
}
```

Keep the state mutated only on the connection owner `EventLoop`.

- [ ] **Step 6: Run targeted test**

Run:

```bash
cmake --build build --target oklib_tcp_read_control_test --parallel
./build/tests/oklib_tcp_read_control_test
```

Expected: test passes.

- [ ] **Step 7: Commit**

```bash
git add include/oklib/net/tcp_connection.h src/net/tcp_connection.cpp tests/net/tcp_read_control_test.cpp tests/CMakeLists.txt
git commit -m "Add TCP read pause and resume"
```

---

### Task 2: Expose Read Control and Cancellation to HTTP Streaming

**Files:**
- Modify: `include/oklib/http/http_request_body_stream.h`
- Modify: `src/http/http_request_body_stream.cpp`
- Modify: `src/http/http_server.cpp`
- Modify: `src/http/http_context.cpp`
- Modify: `include/oklib/http/http_context.h`
- Test: `tests/http/http_upload_writer_test.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing HTTP stream control test**

Create `tests/http/http_upload_writer_test.cpp` with this initial test:

```cpp
#include <oklib/base/thread_pool.h>
#include <oklib/http/http_request_body_stream.h>
#include <oklib/http/http_response_writer.h>
#include <oklib/http/http_server.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

std::string request_once(const std::string& request,
                         const oklib::net::InetAddress& address,
                         oklib::net::EventLoop* loop) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0, "socket succeeds");
  require(::connect(fd, address.sockaddr_ptr(), address.length()) == 0, "connect succeeds");
  require(::write(fd, request.data(), request.size()) == static_cast<ssize_t>(request.size()),
          "write succeeds");
  std::string response;
  char buffer[4096];
  for (;;) {
    const auto n = ::read(fd, buffer, sizeof(buffer));
    if (n > 0) {
      response.append(buffer, static_cast<std::size_t>(n));
      if (response.find("\r\n\r\n") != std::string::npos) {
        break;
      }
    } else {
      break;
    }
  }
  ::close(fd);
  loop->queue_in_loop([loop] { loop->quit(); });
  return response;
}

void test_stream_can_pause_and_resume_reading() {
  oklib::net::EventLoop loop;
  bool saw_body = false;
  bool pause_called = false;
  bool resume_called = false;

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-stream-control-test");
  server.set_streaming_http_callback(
      [&](oklib::http::HttpRequest,
          oklib::http::HttpRequestBodyStream body,
          oklib::http::HttpResponseWriter writer) {
        body.pause_reading();
        pause_called = true;
        loop.run_after(std::chrono::milliseconds(20), [body] { body.resume_reading(); });
        body.set_data_callback([&](std::string_view) {
          saw_body = true;
          resume_called = true;
        });
        body.set_complete_callback([writer = std::move(writer)]() mutable {
          auto response = writer.make_response();
          response.set_status_code(200);
          response.set_body("ok");
          writer.send(std::move(response));
        });
      });
  server.start();

  const std::string body = "abcdef";
  const std::string request =
      "POST / HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Length: " + std::to_string(body.size()) + "\r\n"
      "Connection: close\r\n\r\n" + body;

  std::string response;
  std::thread client([&] { response = request_once(request, server.listen_address(), &loop); });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();

  require(pause_called, "pause_reading was called");
  require(resume_called, "resume_reading allowed body callback");
  require(saw_body, "body was delivered after resume");
  require(response.find("HTTP/1.1 200 OK") != std::string::npos, "response succeeds");
}

}  // namespace

int main() {
  test_stream_can_pause_and_resume_reading();
  return 0;
}
```

- [ ] **Step 2: Register the HTTP upload writer test**

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(oklib_http_upload_writer_test http/http_upload_writer_test.cpp)
target_link_libraries(oklib_http_upload_writer_test PRIVATE oklib::http)
add_test(NAME oklib.http.upload_writer COMMAND oklib_http_upload_writer_test)
```

- [ ] **Step 3: Run to verify failure**

Run:

```bash
cmake --build build --target oklib_http_upload_writer_test --parallel
```

Expected: compile failure because `HttpRequestBodyStream::pause_reading()` and `resume_reading()` do not exist.

- [ ] **Step 4: Add stream read-control API**

Add to `include/oklib/http/http_request_body_stream.h`:

```cpp
using ControlCallback = std::function<void()>;
using CancelCallback = std::function<void()>;

void pause_reading() const;
void resume_reading() const;
void set_cancel_callback(CancelCallback callback) const;
```

Add private/friend-only methods:

```cpp
void set_pause_callback(ControlCallback callback) const;
void set_resume_callback(ControlCallback callback) const;
void on_cancel() const;
```

- [ ] **Step 5: Implement stream control callbacks**

In `src/http/http_request_body_stream.cpp`, extend `State`:

```cpp
ControlCallback pause_callback;
ControlCallback resume_callback;
CancelCallback cancel_callback;
std::atomic_bool canceled{false};
```

Implement:

```cpp
void HttpRequestBodyStream::pause_reading() const {
  ControlCallback callback;
  {
    std::lock_guard lock(state_->mutex);
    callback = state_->pause_callback;
  }
  if (callback) {
    callback();
  }
}

void HttpRequestBodyStream::resume_reading() const {
  ControlCallback callback;
  {
    std::lock_guard lock(state_->mutex);
    callback = state_->resume_callback;
  }
  if (callback) {
    callback();
  }
}
```

Follow the same copy-under-lock pattern used by `on_data()`.

- [ ] **Step 6: Wire HTTP stream to TCP connection**

In `HttpServer::on_streaming_request`, after constructing `HttpRequestBodyStream body_stream`, set:

```cpp
std::weak_ptr<oklib::net::TcpConnection> weak_connection = connection;
body_stream.set_pause_callback([weak_connection] {
  if (auto conn = weak_connection.lock()) {
    conn->pause_reading();
  }
});
body_stream.set_resume_callback([weak_connection] {
  if (auto conn = weak_connection.lock()) {
    conn->resume_reading();
  }
});
```

- [ ] **Step 7: Add cancellation notification**

In `HttpContext`, when a streaming body is active and the connection is closed/reset, call:

```cpp
body_stream_.on_cancel();
```

Add a public method:

```cpp
void cancel_streaming_body();
```

Call it from the server connection-close path before erasing the context.

- [ ] **Step 8: Run targeted tests**

Run:

```bash
cmake --build build --target oklib_http_upload_writer_test --parallel
./build/tests/oklib_http_upload_writer_test
```

Expected: tests pass.

- [ ] **Step 9: Commit**

```bash
git add include/oklib/http/http_request_body_stream.h src/http/http_request_body_stream.cpp src/http/http_server.cpp include/oklib/http/http_context.h src/http/http_context.cpp tests/http/http_upload_writer_test.cpp tests/CMakeLists.txt
git commit -m "Expose HTTP stream read control"
```

---

### Task 3: Implement Upload Block Pool and Dedicated FileWriterPool

**Files:**
- Create: `include/oklib/http/upload_file_writer.h`
- Create: `src/http/upload_file_writer.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/http/http_upload_writer_test.cpp`

- [ ] **Step 1: Extend tests for `.part` rename**

Append to `tests/http/http_upload_writer_test.cpp`:

```cpp
#include <oklib/http/upload_file_writer.h>
#include <filesystem>
#include <fstream>

void test_upload_writer_writes_part_then_renames() {
  const auto dir = std::filesystem::current_path() / "uploads";
  const auto final_path = dir / "writer-test.bin";
  const auto part_path = dir / "writer-test.bin.part";
  std::filesystem::remove(final_path);
  std::filesystem::remove(part_path);

  oklib::http::UploadFileWriterPool pool;
  pool.start(1);

  oklib::http::UploadFileWriterOptions options;
  options.upload_dir = dir;
  options.file_name = "writer-test.bin";
  options.content_type = "application/octet-stream";
  auto session = pool.create_session(options);

  require(session->append("hello "), "first append succeeds");
  require(session->append("world"), "second append succeeds");
  session->finish();
  session->wait_for_test();
  pool.stop();

  require(std::filesystem::exists(final_path), "final file exists");
  require(!std::filesystem::exists(part_path), "part file removed after rename");

  std::ifstream file(final_path, std::ios::binary);
  std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  require(body == "hello world", "file body preserved");
  std::filesystem::remove(final_path);
}
```

Call it from `main()`.

- [ ] **Step 2: Verify test fails**

Run:

```bash
cmake --build build --target oklib_http_upload_writer_test --parallel
```

Expected: compile failure because `upload_file_writer.h` does not exist.

- [ ] **Step 3: Add public upload writer API**

Create `include/oklib/http/upload_file_writer.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "oklib/base/thread_pool.h"

namespace oklib::http {

struct UploadFileWriterOptions {
  std::filesystem::path upload_dir{"uploads"};
  std::string file_name{"upload.bin"};
  std::string content_type{"application/octet-stream"};
  std::size_t block_size{256 * 1024};
  std::size_t high_watermark{8 * 1024 * 1024};
  std::size_t low_watermark{4 * 1024 * 1024};
  std::uint64_t max_upload_size{1024ull * 1024ull * 1024ull};
};

struct UploadFileWriterResult {
  bool ok{false};
  bool canceled{false};
  std::filesystem::path path;
  std::string file_name;
  std::string content_type;
  std::size_t bytes{0};
  std::string error_message;
};

class UploadFileWriterSession;

class UploadFileWriterPool {
 public:
  using CompletionCallback = std::function<void(UploadFileWriterResult)>;
  using WatermarkCallback = std::function<void()>;

  UploadFileWriterPool();
  ~UploadFileWriterPool();

  void start(int thread_count);
  void stop();

  std::shared_ptr<UploadFileWriterSession> create_session(
      UploadFileWriterOptions options,
      CompletionCallback completion = {},
      WatermarkCallback high_watermark = {},
      WatermarkCallback low_watermark = {});

 private:
  oklib::ThreadPool workers_{"upload-file-writer"};
};

class UploadFileWriterSession {
 public:
  virtual ~UploadFileWriterSession() = default;

  virtual bool append(std::string_view data) = 0;
  virtual void finish() = 0;
  virtual void cancel() = 0;

  [[nodiscard]] virtual std::size_t queued_bytes() const = 0;
  [[nodiscard]] virtual std::size_t bytes_written() const = 0;

  virtual void wait_for_test() = 0;
};

}  // namespace oklib::http
```

- [ ] **Step 4: Implement writer pool with owned blocks**

Create `src/http/upload_file_writer.cpp` with a simple first implementation:

```cpp
#include "oklib/http/upload_file_writer.h"

#include "oklib/base/thread_pool.h"

#include <deque>
#include <fstream>
#include <utility>

namespace oklib::http {
namespace {

std::filesystem::path part_path_for(const std::filesystem::path& final_path) {
  return final_path.string() + ".part";
}

}  // namespace

struct UploadFileWriterSessionState {
  UploadFileWriterOptions options;
  UploadFileWriterPool::CompletionCallback completion;
  UploadFileWriterPool::WatermarkCallback high_watermark;
  UploadFileWriterPool::WatermarkCallback low_watermark;
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::string> chunks;
  bool finished{false};
  bool canceled{false};
  bool worker_done{false};
  std::size_t queued{0};
  std::size_t written{0};
};

class UploadFileWriterSessionImpl : public UploadFileWriterSession {
 public:
  explicit UploadFileWriterSessionImpl(std::shared_ptr<UploadFileWriterSessionState> state)
      : state_(std::move(state)) {}

  bool append(std::string_view data) override {
    if (data.empty()) {
      return true;
    }
    UploadFileWriterPool::WatermarkCallback high_callback;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->finished || state_->canceled ||
          state_->written + state_->queued + data.size() > state_->options.max_upload_size) {
        return false;
      }
      state_->chunks.emplace_back(data);
      state_->queued += data.size();
      if (state_->queued >= state_->options.high_watermark) {
        high_callback = state_->high_watermark;
      }
    }
    state_->cv.notify_one();
    if (high_callback) {
      high_callback();
    }
    return true;
  }

  void finish() override {
    {
      std::lock_guard lock(state_->mutex);
      state_->finished = true;
    }
    state_->cv.notify_one();
  }

  void cancel() override {
    {
      std::lock_guard lock(state_->mutex);
      state_->canceled = true;
      state_->finished = true;
    }
    state_->cv.notify_one();
  }

  std::size_t queued_bytes() const override {
    std::lock_guard lock(state_->mutex);
    return state_->queued;
  }

  std::size_t bytes_written() const override {
    std::lock_guard lock(state_->mutex);
    return state_->written;
  }

  void wait_for_test() override {
    std::unique_lock lock(state_->mutex);
    state_->cv.wait(lock, [this] { return state_->worker_done; });
  }

 private:
  std::shared_ptr<UploadFileWriterSessionState> state_;
};

}  // namespace oklib::http
```

Complete `UploadFileWriterPool` with these concrete behaviors:

```cpp
UploadFileWriterPool::UploadFileWriterPool() = default;

UploadFileWriterPool::~UploadFileWriterPool() {
  stop();
}

void UploadFileWriterPool::start(int thread_count) {
  workers_.start(thread_count);
}

void UploadFileWriterPool::stop() {
  workers_.stop();
}

std::shared_ptr<UploadFileWriterSession> UploadFileWriterPool::create_session(
    UploadFileWriterOptions options,
    CompletionCallback completion,
    WatermarkCallback high_watermark,
    WatermarkCallback low_watermark) {
  auto state = std::make_shared<UploadFileWriterSessionState>();
  state->options = std::move(options);
  state->completion = std::move(completion);
  state->high_watermark = std::move(high_watermark);
  state->low_watermark = std::move(low_watermark);
  auto session = std::make_shared<UploadFileWriterSessionImpl>(state);
  workers_.run([state] {
    run_upload_writer_session(state);
  });
  return session;
}
```

Add a private `oklib::ThreadPool workers_{"upload-file-writer"}` field to `UploadFileWriterPool`. Implement `run_upload_writer_session(state)` so it:

- creates `state->options.upload_dir`
- opens `upload_dir / (file_name + ".part")`
- waits on `state->cv`
- pops chunks in FIFO order
- subtracts each chunk from `queued`
- writes bytes to disk
- sets `worker_done = true` and notifies `cv`
- calls `completion(UploadFileWriterResult{...})`
- renames `.part` to the final path only when the session finished without cancel or write error

Keep chunks as owned strings only for this first implementation; Task 4 replaces the queue storage with reusable blocks.

- [ ] **Step 5: Add CMake source**

Add `src/http/upload_file_writer.cpp` to the `oklib_http` source list in `CMakeLists.txt`.

- [ ] **Step 6: Run writer tests**

Run:

```bash
cmake --build build --target oklib_http_upload_writer_test --parallel
./build/tests/oklib_http_upload_writer_test
```

Expected: tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/oklib/http/upload_file_writer.h src/http/upload_file_writer.cpp CMakeLists.txt tests/http/http_upload_writer_test.cpp
git commit -m "Add upload file writer pool"
```

---

### Task 4: Replace Per-Chunk String Allocation With Reusable Blocks

**Files:**
- Modify: `include/oklib/http/upload_file_writer.h`
- Modify: `src/http/upload_file_writer.cpp`
- Test: `tests/http/http_upload_writer_test.cpp`

- [ ] **Step 1: Add block reuse test**

Add a test that appends many small chunks and verifies the session reports bounded allocations:

```cpp
void test_upload_writer_reuses_blocks_for_many_small_chunks() {
  oklib::http::UploadFileWriterPool pool;
  pool.start(1);

  oklib::http::UploadFileWriterOptions options;
  options.file_name = "block-reuse-test.bin";
  options.block_size = 64 * 1024;
  auto session = pool.create_session(options);

  for (int i = 0; i < 1024; ++i) {
    require(session->append("abcd"), "small append succeeds");
  }
  session->finish();
  session->wait_for_test();
  require(session->debug_blocks_allocated_for_test() < 32,
          "block pool avoids one allocation per chunk");
  pool.stop();
  std::filesystem::remove(std::filesystem::path("uploads") / "block-reuse-test.bin");
}
```

- [ ] **Step 2: Add test-only debug accessor**

Add to `UploadFileWriterSession`:

```cpp
[[nodiscard]] std::size_t debug_blocks_allocated_for_test() const;
```

- [ ] **Step 3: Implement block pool**

Replace `std::deque<std::string>` with:

```cpp
struct UploadBlock {
  std::unique_ptr<char[]> data;
  std::size_t size{0};
  std::size_t capacity{0};
};

std::deque<UploadBlock> ready_blocks;
std::vector<UploadBlock> free_blocks;
```

Append logic:

```cpp
UploadBlock block = acquire_block_locked();
const auto copy_size = std::min(data.size(), block.capacity);
std::memcpy(block.data.get(), data.data(), copy_size);
block.size = copy_size;
ready_blocks.push_back(std::move(block));
```

If incoming data is larger than one block, split it into multiple blocks. Return blocks to `free_blocks` after write completes.

- [ ] **Step 4: Run writer tests**

Run:

```bash
cmake --build build --target oklib_http_upload_writer_test --parallel
./build/tests/oklib_http_upload_writer_test
```

Expected: tests pass and block allocation count stays bounded.

- [ ] **Step 5: Commit**

```bash
git add include/oklib/http/upload_file_writer.h src/http/upload_file_writer.cpp tests/http/http_upload_writer_test.cpp
git commit -m "Reuse upload writer blocks"
```

---

### Task 5: Add Queue Watermarks and Backpressure Wiring

**Files:**
- Modify: `src/http/upload_file_writer.cpp`
- Modify: `examples/http_demo_routes.h`
- Test: `tests/http/http_upload_writer_test.cpp`

- [ ] **Step 1: Add watermark callback test**

Add:

```cpp
void test_upload_writer_watermarks_fire_once_per_transition() {
  oklib::http::UploadFileWriterPool pool;
  pool.start(1);

  oklib::http::UploadFileWriterOptions options;
  options.file_name = "watermark-test.bin";
  options.high_watermark = 8;
  options.low_watermark = 4;

  int high_calls = 0;
  int low_calls = 0;
  auto session = pool.create_session(
      options,
      {},
      [&] { ++high_calls; },
      [&] { ++low_calls; });

  require(session->append("12345678"), "append reaches high watermark");
  session->finish();
  session->wait_for_test();
  pool.stop();

  require(high_calls == 1, "high watermark fires once");
  require(low_calls == 1, "low watermark fires after drain");
  std::filesystem::remove(std::filesystem::path("uploads") / "watermark-test.bin");
}
```

- [ ] **Step 2: Implement transition tracking**

Add state:

```cpp
bool above_high_watermark{false};
```

When enqueue crosses high watermark, set `above_high_watermark = true` and fire high callback. When worker drains below low watermark, set `above_high_watermark = false` and fire low callback.

- [ ] **Step 3: Wire demo route to pause/resume**

In `/upload-file-worker`, pass callbacks:

```cpp
auto state = upload_writer_pool.create_session(
    options,
    completion,
    [body_stream] { body_stream.pause_reading(); },
    [body_stream] { body_stream.resume_reading(); });
```

Use the session from the dedicated `UploadFileWriterPool`, not the old local `WorkerUploadState`.

- [ ] **Step 4: Run tests**

Run:

```bash
cmake --build build --target oklib_http_upload_writer_test oklib_http_demo_routes_test --parallel
./build/tests/oklib_http_upload_writer_test
./build/tests/oklib_http_demo_routes_test
```

Expected: tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/http/upload_file_writer.cpp examples/http_demo_routes.h tests/http/http_upload_writer_test.cpp
git commit -m "Add upload writer watermarks"
```

---

### Task 6: Add Cancel Cleanup and Timeout Handling

**Files:**
- Modify: `src/http/upload_file_writer.cpp`
- Modify: `src/http/http_context.cpp`
- Modify: `examples/http_demo_routes.h`
- Test: `tests/http/http_upload_writer_test.cpp`

- [ ] **Step 1: Add cancel cleanup test**

Add:

```cpp
void test_upload_writer_cancel_removes_part_file() {
  const auto dir = std::filesystem::current_path() / "uploads";
  const auto final_path = dir / "cancel-test.bin";
  const auto part_path = dir / "cancel-test.bin.part";
  std::filesystem::remove(final_path);
  std::filesystem::remove(part_path);

  oklib::http::UploadFileWriterPool pool;
  pool.start(1);

  oklib::http::UploadFileWriterOptions options;
  options.upload_dir = dir;
  options.file_name = "cancel-test.bin";
  auto session = pool.create_session(options);
  require(session->append("partial"), "append succeeds before cancel");
  session->cancel();
  session->wait_for_test();
  pool.stop();

  require(!std::filesystem::exists(final_path), "final file not created after cancel");
  require(!std::filesystem::exists(part_path), "part file removed after cancel");
}
```

- [ ] **Step 2: Implement cleanup**

In writer worker completion:

```cpp
if (canceled || !ok) {
  std::filesystem::remove(part_path, ec);
} else {
  std::filesystem::rename(part_path, final_path, ec);
}
```

- [ ] **Step 3: Wire stream cancel**

In `/upload-file-worker`, add:

```cpp
body_stream.set_cancel_callback([session] {
  session->cancel();
});
```

- [ ] **Step 4: Add upload timeout option**

Add to `UploadFileWriterOptions`:

```cpp
std::chrono::milliseconds timeout{std::chrono::minutes(5)};
```

Use `EventLoop::run_after` in the route to cancel the session if it has not completed.

- [ ] **Step 5: Run tests**

Run:

```bash
cmake --build build --target oklib_http_upload_writer_test --parallel
./build/tests/oklib_http_upload_writer_test
```

Expected: tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/oklib/http/upload_file_writer.h src/http/upload_file_writer.cpp src/http/http_context.cpp examples/http_demo_routes.h tests/http/http_upload_writer_test.cpp
git commit -m "Clean up canceled uploads"
```

---

### Task 7: Add Global Limits and Metrics

**Files:**
- Modify: `include/oklib/http/upload_file_writer.h`
- Modify: `src/http/upload_file_writer.cpp`
- Test: `tests/http/http_upload_writer_test.cpp`

- [ ] **Step 1: Add options and snapshot types**

Add:

```cpp
struct UploadFileWriterPoolOptions {
  std::size_t max_active_uploads{128};
  std::size_t max_total_queued_bytes{512 * 1024 * 1024};
};

struct UploadFileWriterStats {
  std::size_t active_uploads{0};
  std::size_t total_queued_bytes{0};
  std::uint64_t completed_uploads{0};
  std::uint64_t failed_uploads{0};
  std::uint64_t canceled_uploads{0};
};
```

Replace the default pool constructor with an options-aware constructor:

```cpp
explicit UploadFileWriterPool(UploadFileWriterPoolOptions options = {});
[[nodiscard]] UploadFileWriterStats stats() const;
```

- [ ] **Step 2: Add limit test**

Add:

```cpp
void test_upload_writer_rejects_when_active_limit_reached() {
  oklib::http::UploadFileWriterPoolOptions pool_options;
  pool_options.max_active_uploads = 1;
  oklib::http::UploadFileWriterPool pool(pool_options);
  pool.start(1);

  oklib::http::UploadFileWriterOptions options;
  options.file_name = "limit-a.bin";
  auto first = pool.create_session(options);
  options.file_name = "limit-b.bin";
  auto second = pool.create_session(options);

  require(first != nullptr, "first session accepted");
  require(second == nullptr, "second session rejected by active limit");
  first->cancel();
  first->wait_for_test();
  pool.stop();
}
```

- [ ] **Step 3: Implement global counters**

Maintain pool-level counters under a mutex. Reject `create_session()` by returning `nullptr` when limits are exceeded.

- [ ] **Step 4: Run tests**

Run:

```bash
cmake --build build --target oklib_http_upload_writer_test --parallel
./build/tests/oklib_http_upload_writer_test
```

Expected: tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/oklib/http/upload_file_writer.h src/http/upload_file_writer.cpp tests/http/http_upload_writer_test.cpp
git commit -m "Add upload writer limits and stats"
```

---

### Task 8: Add Streaming Multipart Parser

**Files:**
- Create: `include/oklib/http/streaming_multipart_parser.h`
- Create: `src/http/streaming_multipart_parser.cpp`
- Modify: `CMakeLists.txt`
- Modify: `examples/http_demo_routes.h`
- Test: `tests/http/streaming_multipart_parser_test.cpp`

- [ ] **Step 1: Add parser test**

Create `tests/http/streaming_multipart_parser_test.cpp`:

```cpp
#include <oklib/http/streaming_multipart_parser.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

void test_streaming_parser_emits_file_chunks() {
  std::vector<std::string> chunks;
  std::string filename;
  oklib::http::StreamingMultipartParser parser(
      "----oklib",
      [&](const oklib::http::StreamingMultipartPart& part) {
        filename = part.filename;
      },
      [&](std::string_view data) {
        chunks.emplace_back(data);
      });

  const std::string body =
      "------oklib\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"demo.txt\"\r\n"
      "Content-Type: text/plain\r\n"
      "\r\n"
      "hello"
      "\r\n------oklib--\r\n";

  require(parser.append(body.substr(0, 20)), "first append accepted");
  require(parser.append(body.substr(20)), "second append accepted");
  require(parser.finish(), "parser finishes");
  require(filename == "demo.txt", "filename parsed");
  require(!chunks.empty(), "file data emitted");
}

}  // namespace

int main() {
  test_streaming_parser_emits_file_chunks();
  return 0;
}
```

- [ ] **Step 2: Register test and source**

Add source to `CMakeLists.txt`:

```cmake
src/http/streaming_multipart_parser.cpp
```

Add test to `tests/CMakeLists.txt`:

```cmake
add_executable(oklib_http_streaming_multipart_parser_test http/streaming_multipart_parser_test.cpp)
target_link_libraries(oklib_http_streaming_multipart_parser_test PRIVATE oklib::http)
add_test(NAME oklib.http.streaming_multipart_parser COMMAND oklib_http_streaming_multipart_parser_test)
```

- [ ] **Step 3: Implement parser state machine**

Support:

- boundary detection across chunk boundaries
- part header parsing with existing `HttpHeaders`
- decoded `filename` using existing URL encoding helper behavior
- file chunk callback without whole-body buffering
- small field buffering with a per-field limit
- malformed boundary/header errors

- [ ] **Step 4: Wire multipart upload route**

In `/upload-file-worker`, for `multipart/form-data`, use `StreamingMultipartParser` and feed file part bytes into `UploadFileWriterSession::append()`.

- [ ] **Step 5: Run parser and demo route tests**

Run:

```bash
cmake --build build --target oklib_http_streaming_multipart_parser_test oklib_http_demo_routes_test --parallel
./build/tests/oklib_http_streaming_multipart_parser_test
./build/tests/oklib_http_demo_routes_test
```

Expected: tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/oklib/http/streaming_multipart_parser.h src/http/streaming_multipart_parser.cpp CMakeLists.txt tests/CMakeLists.txt tests/http/streaming_multipart_parser_test.cpp examples/http_demo_routes.h
git commit -m "Add streaming multipart upload parser"
```

---

### Task 9: Add Upload Benchmark and Documentation

**Files:**
- Create: `examples/http_upload_benchmark.cpp`
- Modify: `examples/CMakeLists.txt`
- Modify: `docs/http11_progress.md`
- Create: `docs/upload_pipeline.md`

- [ ] **Step 1: Add benchmark example**

Create an example that runs local upload clients against `/upload-file-worker` with configurable:

```text
--clients
--requests
--body-size
--chunk-size
--url
```

Output:

```text
requests=100 clients=8 body_size=1048576 seconds=1.23 throughput_mb_s=812.3 avg_ms=42 p95_ms=88 p99_ms=120
```

- [ ] **Step 2: Register benchmark**

Add to `examples/CMakeLists.txt`:

```cmake
add_executable(oklib_http_upload_benchmark http_upload_benchmark.cpp)
target_link_libraries(oklib_http_upload_benchmark PRIVATE oklib::http)
```

- [ ] **Step 3: Document production guidance**

Create `docs/upload_pipeline.md` explaining:

- raw upload path
- multipart streaming path
- recommended writer thread counts
- per-upload queue watermarks
- global queued byte limits
- why increasing worker threads alone is insufficient
- why one owned copy from socket buffer into upload block is expected

- [ ] **Step 4: Run benchmark smoke**

Run:

```bash
cmake --build build --target oklib_http_upload_benchmark --parallel
./build/examples/oklib_http_upload_benchmark --requests 4 --clients 2 --body-size 65536
```

Expected: benchmark completes and reports throughput.

- [ ] **Step 5: Commit**

```bash
git add examples/http_upload_benchmark.cpp examples/CMakeLists.txt docs/http11_progress.md docs/upload_pipeline.md
git commit -m "Add upload benchmark documentation"
```

---

## Verification Checklist

Run after each task:

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run after tasks touching TLS-shared HTTP code:

```bash
cmake --build build-tls --parallel
ctest --test-dir build-tls --output-on-failure
```

Run before considering the upload pipeline complete:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake -S . -B build-tls -DOKLIB_ENABLE_TLS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-tls --parallel
ctest --test-dir build-tls --output-on-failure
./build/examples/oklib_http_upload_benchmark --requests 20 --clients 4 --body-size 1048576
```

## Completion Criteria

- Upload body file writes do not run on I/O threads.
- Raw upload path does not buffer whole files in memory.
- Multipart upload path supports streaming file parts without whole-body buffering.
- Per-upload and global queued bytes are bounded.
- High watermark pauses socket reads and low watermark resumes socket reads.
- Canceled uploads remove `.part` files.
- Successful uploads atomically rename `.part` to final path.
- Tests cover success, limit rejection, cancellation, watermarks, and route integration.
- Benchmark demonstrates stable memory behavior under concurrent uploads.
