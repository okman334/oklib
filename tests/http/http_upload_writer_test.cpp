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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

void test_upload_writer_assigns_unique_paths_for_concurrent_same_name_uploads() {
  const auto dir = std::filesystem::current_path() / "uploads";
  const auto first_path = dir / "upload.bin";
  const auto second_path = dir / "upload-1.bin";
  std::filesystem::remove(first_path);
  std::filesystem::remove(second_path);
  std::filesystem::remove(first_path.string() + ".part");
  std::filesystem::remove(second_path.string() + ".part");

  oklib::http::UploadFileWriterPool pool;
  pool.start(2);

  std::mutex mutex;
  std::vector<oklib::http::UploadFileWriterResult> results;
  auto completion = [&](oklib::http::UploadFileWriterResult result) {
    std::lock_guard lock(mutex);
    results.push_back(std::move(result));
  };

  oklib::http::UploadFileWriterOptions options;
  options.upload_dir = dir;

  auto first = pool.create_session(options, completion);
  auto second = pool.create_session(options, completion);

  require(first != nullptr, "first same-name session created");
  require(second != nullptr, "second same-name session created");
  require(first->append("first"), "first body appended");
  require(second->append("second"), "second body appended");
  first->finish();
  second->finish();
  first->wait_for_test();
  second->wait_for_test();
  pool.stop();

  require(results.size() == 2, "both same-name uploads completed");
  require(results[0].ok && results[1].ok, "both same-name uploads succeeded");
  require(results[0].path != results[1].path, "same-name uploads receive unique paths");
  require(std::filesystem::exists(results[0].path), "first unique upload path exists");
  require(std::filesystem::exists(results[1].path), "second unique upload path exists");

  const auto first_body = read_file(results[0].path);
  const auto second_body = read_file(results[1].path);
  require((first_body == "first" && second_body == "second") ||
              (first_body == "second" && second_body == "first"),
          "same-name upload bodies are not mixed");
  require((results[0].file_name == "upload.bin" && results[1].file_name == "upload-1.bin") ||
              (results[0].file_name == "upload-1.bin" && results[1].file_name == "upload.bin"),
          "same-name uploads report unique file names");

  std::filesystem::remove(results[0].path);
  std::filesystem::remove(results[1].path);
}

void test_upload_writer_can_overwrite_existing_file_when_requested() {
  const auto dir = std::filesystem::current_path() / "uploads";
  const auto final_path = dir / "overwrite-existing.bin";
  std::filesystem::create_directories(dir);
  {
    std::ofstream file(final_path, std::ios::binary | std::ios::trunc);
    require(file.is_open(), "existing overwrite target opens");
    file << "old";
  }
  std::filesystem::remove(dir / "overwrite-existing-1.bin");

  oklib::http::UploadFileWriterPool pool;
  pool.start(1);

  oklib::http::UploadFileWriterOptions options;
  options.upload_dir = dir;
  options.file_name = "overwrite-existing.bin";
  options.overwrite_existing = true;

  oklib::http::UploadFileWriterResult result;
  auto session = pool.create_session(options, [&](oklib::http::UploadFileWriterResult value) {
    result = std::move(value);
  });

  require(session != nullptr, "overwrite session created");
  require(session->append("new"), "overwrite body appended");
  session->finish();
  session->wait_for_test();
  pool.stop();

  require(result.ok, "overwrite upload succeeds");
  require(result.file_name == "overwrite-existing.bin", "overwrite keeps requested file name");
  require(result.path == final_path, "overwrite writes requested final path");
  require(read_file(final_path) == "new", "overwrite replaces existing file");
  require(!std::filesystem::exists(dir / "overwrite-existing-1.bin"),
          "overwrite does not allocate unique backup name");

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

void test_upload_writer_reuses_blocks_across_finished_sessions() {
  const auto dir = std::filesystem::current_path() / "uploads";
  const auto first_path = dir / "cross-session-reuse-a.bin";
  const auto second_path = dir / "cross-session-reuse-b.bin";
  std::filesystem::remove(first_path);
  std::filesystem::remove(second_path);

  oklib::http::UploadFileWriterPool pool;
  pool.start(1);

  oklib::http::UploadFileWriterOptions options;
  options.upload_dir = dir;
  options.file_name = "cross-session-reuse-a.bin";
  options.block_size = 1024;
  auto first = pool.create_session(options);
  require(first != nullptr, "first cross-session reuse session created");
  require(first->append(std::string(4096, 'a')), "first cross-session body appended");
  first->finish();
  first->wait_for_test();
  require(first->debug_blocks_allocated_for_test() == 4,
          "first cross-session upload allocates expected blocks");

  options.file_name = "cross-session-reuse-b.bin";
  auto second = pool.create_session(options);
  require(second != nullptr, "second cross-session reuse session created");
  require(second->append(std::string(4096, 'b')), "second cross-session body appended");
  second->finish();
  second->wait_for_test();
  pool.stop();

  require(second->debug_blocks_allocated_for_test() == 0,
          "second cross-session upload reuses pool cached blocks");
  require(read_file(first_path) == std::string(4096, 'a'),
          "first cross-session body saved");
  require(read_file(second_path) == std::string(4096, 'b'),
          "second cross-session body saved");

  std::filesystem::remove(first_path);
  std::filesystem::remove(second_path);
}

void test_upload_writer_pool_cache_size_can_be_disabled() {
  const auto dir = std::filesystem::current_path() / "uploads";
  const auto first_path = dir / "disabled-cache-a.bin";
  const auto second_path = dir / "disabled-cache-b.bin";
  std::filesystem::remove(first_path);
  std::filesystem::remove(second_path);

  oklib::http::UploadFileWriterPoolOptions pool_options;
  pool_options.max_cached_block_bytes = 0;
  oklib::http::UploadFileWriterPool pool(pool_options);
  pool.start(1);

  oklib::http::UploadFileWriterOptions options;
  options.upload_dir = dir;
  options.file_name = "disabled-cache-a.bin";
  options.block_size = 1024;
  auto first = pool.create_session(options);
  require(first != nullptr, "first disabled-cache session created");
  require(first->append(std::string(4096, 'a')), "first disabled-cache body appended");
  first->finish();
  first->wait_for_test();

  options.file_name = "disabled-cache-b.bin";
  auto second = pool.create_session(options);
  require(second != nullptr, "second disabled-cache session created");
  require(second->append(std::string(4096, 'b')), "second disabled-cache body appended");
  second->finish();
  second->wait_for_test();
  pool.stop();

  require(second->debug_blocks_allocated_for_test() == 4,
          "disabled pool cache forces second session to allocate blocks");

  std::filesystem::remove(first_path);
  std::filesystem::remove(second_path);
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

void test_stream_callbacks_release_self_references_after_complete() {
  using namespace std::chrono_literals;

  oklib::net::EventLoop loop;
  std::weak_ptr<int> marker;

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-stream-release-test");
  server.set_streaming_http_callback(
      [&](oklib::http::HttpRequest,
          oklib::http::HttpRequestBodyStream body,
          oklib::http::HttpResponseWriter writer) mutable {
        auto owned_marker = std::make_shared<int>(42);
        marker = owned_marker;
        body.set_data_callback([body, owned_marker](std::string_view) {
          (void)body.reading_paused();
          (void)owned_marker;
        });
        body.set_complete_callback(
            [body, owned_marker, writer = std::move(writer)]() mutable {
              (void)body.reading_paused();
              (void)owned_marker;
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
    write_all(fd,
              "POST /release HTTP/1.1\r\n"
              "Host: localhost\r\n"
              "Content-Length: 4\r\n"
              "Connection: close\r\n\r\n"
              "body");
    response = read_response_until_close(fd);
    ::close(fd);
  });

  loop.run_after(500ms, [&] { loop.quit(); });
  loop.loop();
  client.join();

  require(response.find("HTTP/1.1 200 OK") != std::string::npos,
          "self-referencing stream request completes");
  require(marker.expired(), "stream callbacks release self-referencing captures after complete");
}

}  // namespace

int main() {
  test_upload_writer_writes_part_then_renames();
  test_upload_writer_assigns_unique_paths_for_concurrent_same_name_uploads();
  test_upload_writer_can_overwrite_existing_file_when_requested();
  test_upload_writer_cancel_removes_part_file();
  test_upload_writer_reuses_blocks_for_many_small_chunks();
  test_upload_writer_reuses_blocks_across_finished_sessions();
  test_upload_writer_pool_cache_size_can_be_disabled();
  test_upload_writer_watermarks_fire_once_per_transition();
  test_upload_writer_rejects_limits_and_counts_failures();
  test_stream_pause_resume_and_cancel();
  test_stream_callbacks_release_self_references_after_complete();
  return 0;
}
