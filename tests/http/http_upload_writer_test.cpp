#include <oklib/http/http_request_body_stream.h>
#include <oklib/http/http_request.h>
#include <oklib/http/http_response.h>
#include <oklib/http/http_response_writer.h>
#include <oklib/http/http_server.h>
#include <oklib/http/upload_file_writer.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

int connect_socket(const oklib::net::InetAddress& address) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0, "socket succeeds");
  require(::connect(fd, address.sockaddr_ptr(), address.length()) == 0, "connect succeeds");
  return fd;
}

void write_all(int fd, const std::string& data) {
  std::size_t written = 0;
  while (written < data.size()) {
    const auto n = ::write(fd, data.data() + written, data.size() - written);
    require(n > 0, "write succeeds");
    written += static_cast<std::size_t>(n);
  }
}

std::string read_response_until_close(int fd) {
  std::string response;
  char buffer[4096];
  for (;;) {
    const auto n = ::read(fd, buffer, sizeof(buffer));
    if (n > 0) {
      response.append(buffer, static_cast<std::size_t>(n));
      continue;
    }
    break;
  }
  return response;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  require(file.is_open(), "file opens");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

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

  require(session != nullptr, "writer session created");
  require(session->append("hello "), "first append succeeds");
  require(session->append("world"), "second append succeeds");
  session->finish();
  session->wait_for_test();
  pool.stop();

  require(std::filesystem::exists(final_path), "final file exists");
  require(!std::filesystem::exists(part_path), "part file removed after rename");
  require(read_file(final_path) == "hello world", "file body preserved");

  const auto stats = pool.stats();
  require(stats.active_uploads == 0, "stats active uploads drained");
  require(stats.completed_uploads == 1, "stats completed upload counted");

  std::filesystem::remove(final_path);
}

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

  require(session != nullptr, "writer session created");
  require(session->append("partial"), "append succeeds before cancel");
  session->cancel();
  session->wait_for_test();
  pool.stop();

  require(!std::filesystem::exists(final_path), "final file not created after cancel");
  require(!std::filesystem::exists(part_path), "part file removed after cancel");
  require(pool.stats().canceled_uploads == 1, "stats canceled upload counted");
}

void test_upload_writer_reuses_blocks_for_many_small_chunks() {
  const auto dir = std::filesystem::current_path() / "uploads";
  const auto final_path = dir / "block-reuse-test.bin";
  std::filesystem::remove(final_path);

  oklib::http::UploadFileWriterPool pool;
  pool.start(1);

  oklib::http::UploadFileWriterOptions options;
  options.upload_dir = dir;
  options.file_name = "block-reuse-test.bin";
  options.block_size = 64 * 1024;
  auto session = pool.create_session(options);

  require(session != nullptr, "writer session created");
  for (int i = 0; i < 1024; ++i) {
    require(session->append("abcd"), "small append succeeds");
  }
  session->finish();
  session->wait_for_test();
  pool.stop();

  require(session->debug_blocks_allocated_for_test() < 32,
          "block pool avoids one allocation per small chunk");
  require(session->bytes_written() == 4096, "all small chunks were written");

  std::filesystem::remove(final_path);
}

void test_upload_writer_watermarks_fire_once_per_transition() {
  const auto dir = std::filesystem::current_path() / "uploads";
  const auto final_path = dir / "watermark-test.bin";
  std::filesystem::remove(final_path);

  oklib::http::UploadFileWriterPool pool;
  pool.start(1);

  oklib::http::UploadFileWriterOptions options;
  options.upload_dir = dir;
  options.file_name = "watermark-test.bin";
  options.block_size = 8;
  options.high_watermark = 8;
  options.low_watermark = 4;

  std::atomic<int> high_calls{0};
  std::atomic<int> low_calls{0};
  auto session = pool.create_session(
      options,
      {},
      [&] { ++high_calls; },
      [&] { ++low_calls; });

  require(session != nullptr, "writer session created");
  require(session->append("12345678"), "append reaches high watermark");
  session->finish();
  session->wait_for_test();
  pool.stop();

  require(high_calls.load() == 1, "high watermark fires once");
  require(low_calls.load() == 1, "low watermark fires after drain");

  std::filesystem::remove(final_path);
}

void test_upload_writer_rejects_limits_and_counts_failures() {
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
  require(pool.stats().canceled_uploads == 1, "active-limit test canceled first session");

  oklib::http::UploadFileWriterPool size_pool;
  size_pool.start(1);
  oklib::http::UploadFileWriterOptions size_options;
  size_options.file_name = "size-limit.bin";
  size_options.max_upload_size = 4;
  auto limited = size_pool.create_session(size_options);
  require(limited != nullptr, "size-limited session accepted");
  require(!limited->append("12345"), "append beyond max upload size is rejected");
  limited->wait_for_test();
  size_pool.stop();
  require(size_pool.stats().failed_uploads == 1, "size-limit failure counted");
  std::filesystem::remove(std::filesystem::path("uploads") / "size-limit.bin");
  std::filesystem::remove(std::filesystem::path("uploads") / "size-limit.bin.part");

  oklib::http::UploadFileWriterPoolOptions queue_pool_options;
  queue_pool_options.max_total_queued_bytes = 4;
  oklib::http::UploadFileWriterPool queue_pool(queue_pool_options);
  queue_pool.start(1);
  oklib::http::UploadFileWriterOptions queue_options;
  queue_options.file_name = "queue-limit.bin";
  auto queued = queue_pool.create_session(queue_options);
  require(queued != nullptr, "queue-limited session accepted");
  require(!queued->append("12345"), "append beyond global queued bytes is rejected");
  queued->wait_for_test();
  queue_pool.stop();
  require(queue_pool.stats().failed_uploads == 1, "global queue-limit failure counted");
  require(queue_pool.stats().total_queued_bytes == 0, "global queued bytes released after failure");
  std::filesystem::remove(std::filesystem::path("uploads") / "queue-limit.bin");
  std::filesystem::remove(std::filesystem::path("uploads") / "queue-limit.bin.part");
}

void test_stream_pause_resume_and_cancel() {
  using namespace std::chrono_literals;

  oklib::net::EventLoop loop;
  std::atomic<bool> saw_body{false};
  std::atomic<bool> checked_paused{false};
  std::atomic<bool> canceled{false};

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-stream-control-test");
  server.set_streaming_http_callback(
      [&](oklib::http::HttpRequest request,
          oklib::http::HttpRequestBodyStream body,
          oklib::http::HttpResponseWriter writer) mutable {
        if (request.path() == "/cancel") {
          body.set_cancel_callback([&] {
            canceled = true;
            loop.quit();
          });
          return;
        }

        body.pause_reading();
        loop.run_after(40ms, [&, body] {
          require(!saw_body.load(), "paused body stream does not deliver buffered body");
          checked_paused = true;
          body.resume_reading();
        });
        body.set_data_callback([&](std::string_view) {
          saw_body = true;
        });
        body.set_complete_callback([writer = std::move(writer)]() mutable {
          auto response = writer.make_response();
          response.set_status_code(200);
          response.set_body("ok");
          writer.send(std::move(response));
        });
      });
  server.start();

  std::string response;
  std::thread client([&] {
    const int fd = connect_socket(server.listen_address());
    const std::string body = "abcdef";
    write_all(fd,
              std::string("POST /pause HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Content-Length: ") +
                  std::to_string(body.size()) + "\r\n"
                  "Connection: close\r\n\r\n" +
                  body);
    response = read_response_until_close(fd);
    ::close(fd);

    const int cancel_fd = connect_socket(server.listen_address());
    write_all(cancel_fd,
              "POST /cancel HTTP/1.1\r\n"
              "Host: localhost\r\n"
              "Content-Length: 64\r\n"
              "Connection: close\r\n\r\n"
              "partial");
    ::close(cancel_fd);
  });
  loop.run_after(2s, [&] { loop.quit(); });
  loop.loop();
  client.join();

  require(checked_paused.load(), "pause checkpoint ran");
  require(saw_body.load(), "body was delivered after resume");
  require(response.find("HTTP/1.1 200 OK") != std::string::npos, "response sent after body completion");
  require(canceled.load(), "stream cancel callback runs on client disconnect");
}

}  // namespace

int main() {
  test_upload_writer_writes_part_then_renames();
  test_upload_writer_cancel_removes_part_file();
  test_upload_writer_reuses_blocks_for_many_small_chunks();
  test_upload_writer_watermarks_fire_once_per_transition();
  test_upload_writer_rejects_limits_and_counts_failures();
  test_stream_pause_resume_and_cancel();
  return 0;
}
