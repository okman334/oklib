#include <http_demo_routes.h>

#include <oklib/base/thread_pool.h>
#include <oklib/http/http_server.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
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

std::string request_once(const std::string& request,
                         const oklib::net::InetAddress& address,
                         oklib::net::EventLoop* loop,
                         const std::function<bool(const std::string&)>& done = {}) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0, "socket succeeds");
  require(::connect(fd, address.sockaddr_ptr(), address.length()) == 0, "connect succeeds");
  require(::write(fd, request.data(), request.size()) == static_cast<ssize_t>(request.size()),
          "write request succeeds");

  std::string response;
  char buffer[4096];
  for (;;) {
    const auto n = ::read(fd, buffer, sizeof(buffer));
    if (n > 0) {
      response.append(buffer, static_cast<std::size_t>(n));
      if (done && done(response)) {
        break;
      }
      if (!done && response.find("\r\n\r\n") != std::string::npos &&
          response.find("\"bytes\"") != std::string::npos) {
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

void test_ping_route_returns_pong() {
  oklib::net::EventLoop loop;
  oklib::ThreadPool workers("http-demo-routes-ping-test-workers");
  workers.start(1);

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-demo-routes-ping-test");
  oklib::examples::install_http_demo_routes(server, workers);
  server.start();

  std::string response;
  std::thread client([&] {
    response = request_once(
        "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        server.listen_address(),
        &loop,
        [](const std::string& value) { return value.find("pong\n") != std::string::npos; });
  });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();
  workers.stop();

  require(response.find("HTTP/1.1 200 OK") != std::string::npos,
          "ping route returns 200");
  require(response.find("pong\n") != std::string::npos, "ping route returns pong body");
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  require(file.is_open(), "uploaded file opens");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void test_upload_file_route_saves_raw_jpeg_body() {
  const std::string file_name = "oklib-demo-routes-upload-test.jpg";
  const std::filesystem::path upload_path = std::filesystem::current_path() / "uploads" / file_name;
  std::filesystem::remove(upload_path);

  oklib::net::EventLoop loop;
  oklib::ThreadPool workers("http-demo-routes-test-workers");
  workers.start(1);

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-demo-routes-upload-test");
  oklib::examples::install_http_demo_routes(server, workers);
  server.start();

  const std::string jpg_body = "\xff\xd8\xff\xe0oklib-test-jpg\xff\xd9";
  const std::string request =
      "POST /upload-file?name=" + file_name + " HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: " + std::to_string(jpg_body.size()) + "\r\n"
      "Connection: close\r\n\r\n" +
      jpg_body;

  std::string response;
  std::thread client([&] {
    response = request_once(request, server.listen_address(), &loop);
  });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();
  workers.stop();

  require(response.find("HTTP/1.1 201 Created") != std::string::npos,
          "upload route returns 201");
  require(response.find("\"file\":\"" + file_name + "\"") != std::string::npos,
          "upload response includes file name");
  require(response.find("\"bytes\":" + std::to_string(jpg_body.size())) != std::string::npos,
          "upload response includes byte count");
  require(read_file(upload_path) == jpg_body, "uploaded jpg body is saved byte-for-byte");

  std::filesystem::remove(upload_path);
}

void test_upload_file_route_saves_multipart_mp4_file() {
  const std::string file_name = "oklib-demo-routes-upload-test.mp4";
  const std::filesystem::path upload_path = std::filesystem::current_path() / "uploads" / file_name;
  std::filesystem::remove(upload_path);

  oklib::net::EventLoop loop;
  oklib::ThreadPool workers("http-demo-routes-multipart-test-workers");
  workers.start(1);

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-demo-routes-multipart-upload-test");
  oklib::examples::install_http_demo_routes(server, workers);
  server.start();

  const std::string boundary = "----oklib-demo-routes-boundary";
  const std::string mp4_body = std::string("\0\0\0\x18", 4) + "ftypmp42oklib-demo";
  const std::string multipart_body =
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"title\"\r\n"
      "\r\n"
      "demo clip\r\n"
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"" + file_name + "\"\r\n"
      "Content-Type: video/mp4\r\n"
      "\r\n" +
      mp4_body +
      "\r\n"
      "--" + boundary + "--\r\n";
  const std::string request =
      "POST /upload-file HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"
      "Content-Length: " + std::to_string(multipart_body.size()) + "\r\n"
      "Connection: close\r\n\r\n" +
      multipart_body;

  std::string response;
  std::thread client([&] {
    response = request_once(request, server.listen_address(), &loop);
  });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();
  workers.stop();

  require(response.find("HTTP/1.1 201 Created") != std::string::npos,
          "multipart upload route returns 201");
  require(response.find("\"file\":\"" + file_name + "\"") != std::string::npos,
          "multipart upload response includes file name");
  require(response.find("\"content_type\":\"video/mp4\"") != std::string::npos,
          "multipart upload response includes file content type");
  require(response.find("\"bytes\":" + std::to_string(mp4_body.size())) != std::string::npos,
          "multipart upload response includes file byte count");
  require(response.find("Access-Control-Allow-Origin: *") != std::string::npos,
          "multipart upload response allows browser demo CORS");
  require(read_file(upload_path) == mp4_body, "uploaded mp4 body is saved byte-for-byte");

  std::filesystem::remove(upload_path);
}

void test_upload_file_route_decodes_percent_encoded_utf8_filename() {
  const std::string file_name = "梨子芥菜 - 情网(烟嗓版).mp4";
  const std::string encoded_name =
      "%E6%A2%A8%E5%AD%90%E8%8A%A5%E8%8F%9C%20-%20"
      "%E6%83%85%E7%BD%91%28%E7%83%9F%E5%97%93%E7%89%88%29.mp4";
  const std::filesystem::path upload_path = std::filesystem::current_path() / "uploads" / file_name;
  std::filesystem::remove(upload_path);

  oklib::net::EventLoop loop;
  oklib::ThreadPool workers("http-demo-routes-utf8-upload-test-workers");
  workers.start(1);

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-demo-routes-utf8-upload-test");
  oklib::examples::install_http_demo_routes(server, workers);
  server.start();

  const std::string boundary = "----oklib-demo-routes-utf8-boundary";
  const std::string mp4_body = std::string("\0\0\0\x18", 4) + "ftypmp42oklib-utf8-demo";
  const std::string multipart_body =
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"" + encoded_name + "\"\r\n"
      "Content-Type: video/mp4\r\n"
      "\r\n" +
      mp4_body +
      "\r\n"
      "--" + boundary + "--\r\n";
  const std::string request =
      "POST /upload-file HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"
      "Content-Length: " + std::to_string(multipart_body.size()) + "\r\n"
      "Connection: close\r\n\r\n" +
      multipart_body;

  std::string response;
  std::thread client([&] {
    response = request_once(request, server.listen_address(), &loop);
  });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();
  workers.stop();

  require(response.find("HTTP/1.1 201 Created") != std::string::npos,
          "utf8 multipart upload route returns 201");
  require(response.find("\"file\":\"" + file_name + "\"") != std::string::npos,
          "utf8 multipart upload response includes decoded file name");
  require(read_file(upload_path) == mp4_body,
          "utf8 multipart upload body is saved under decoded file name");

  std::filesystem::remove(upload_path);
}

void test_upload_file_worker_route_saves_raw_body() {
  const std::string file_name = "oklib-demo-routes-worker-upload-test.bin";
  const std::filesystem::path upload_path = std::filesystem::current_path() / "uploads" / file_name;
  std::filesystem::remove(upload_path);

  oklib::net::EventLoop loop;
  oklib::ThreadPool workers("http-demo-routes-worker-upload-test-workers");
  workers.start(2);

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-demo-routes-worker-upload-test");
  oklib::examples::install_http_demo_routes(server, workers);
  server.start();

  const std::string body = "worker-upload-body-0123456789";
  const std::string request =
      "POST /upload-file-worker?name=" + file_name + " HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/octet-stream\r\n"
      "Content-Length: " + std::to_string(body.size()) + "\r\n"
      "Connection: close\r\n\r\n" +
      body;

  std::string response;
  std::thread client([&] {
    response = request_once(request, server.listen_address(), &loop);
  });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();
  workers.stop();

  require(response.find("HTTP/1.1 201 Created") != std::string::npos,
          "worker upload route returns 201");
  require(response.find("\"file\":\"" + file_name + "\"") != std::string::npos,
          "worker upload response includes file name");
  require(response.find("\"mode\":\"worker\"") != std::string::npos,
          "worker upload response identifies worker mode");
  require(response.find("\"bytes\":" + std::to_string(body.size())) != std::string::npos,
          "worker upload response includes byte count");
  require(read_file(upload_path) == body, "worker uploaded body is saved byte-for-byte");

  std::filesystem::remove(upload_path);
}

void test_upload_file_worker_route_saves_multipart_body() {
  const std::string file_name = "oklib-demo-routes-worker-multipart-test.mp4";
  const std::filesystem::path upload_path = std::filesystem::current_path() / "uploads" / file_name;
  std::filesystem::remove(upload_path);

  oklib::net::EventLoop loop;
  oklib::ThreadPool workers("http-demo-routes-worker-multipart-test-workers");
  workers.start(2);

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-demo-routes-worker-multipart-test");
  oklib::examples::install_http_demo_routes(server, workers);
  server.start();

  const std::string boundary = "----oklib-worker-multipart-boundary";
  const std::string mp4_body = std::string("\0\0\0\x18", 4) + "ftypmp42oklib-worker";
  const std::string multipart_body =
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"" + file_name + "\"\r\n"
      "Content-Type: video/mp4\r\n"
      "\r\n" +
      mp4_body +
      "\r\n"
      "--" + boundary + "--\r\n";
  const std::string request =
      "POST /upload-file-worker HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"
      "Content-Length: " + std::to_string(multipart_body.size()) + "\r\n"
      "Connection: close\r\n\r\n" +
      multipart_body;

  std::string response;
  std::thread client([&] {
    response = request_once(request, server.listen_address(), &loop);
  });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();
  workers.stop();

  require(response.find("HTTP/1.1 201 Created") != std::string::npos,
          "worker multipart upload route returns 201");
  require(response.find("\"file\":\"" + file_name + "\"") != std::string::npos,
          "worker multipart response includes file name");
  require(response.find("\"content_type\":\"video/mp4\"") != std::string::npos,
          "worker multipart response includes content type");
  require(response.find("\"bytes\":" + std::to_string(mp4_body.size())) != std::string::npos,
          "worker multipart response includes byte count");
  require(read_file(upload_path) == mp4_body,
          "worker multipart uploaded body is saved byte-for-byte");

  std::filesystem::remove(upload_path);
}

void test_upload_file_route_handles_cors_preflight() {
  oklib::net::EventLoop loop;
  oklib::ThreadPool workers("http-demo-routes-cors-test-workers");
  workers.start(1);

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-demo-routes-cors-test");
  oklib::examples::install_http_demo_routes(server, workers);
  server.start();

  std::string response;
  std::thread client([&] {
    response = request_once(
        "OPTIONS /upload-file HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Origin: http://127.0.0.1:4173\r\n"
        "Access-Control-Request-Method: POST\r\n"
        "Access-Control-Request-Headers: Content-Type\r\n"
        "Connection: close\r\n\r\n",
        server.listen_address(),
        &loop,
        [](const std::string& value) {
          return value.find("Access-Control-Allow-Methods: POST, OPTIONS") != std::string::npos;
        });
  });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();
  workers.stop();

  require(response.find("HTTP/1.1 204 No Content") != std::string::npos,
          "upload preflight returns 204");
  require(response.find("Access-Control-Allow-Origin: *") != std::string::npos,
          "upload preflight allows origin");
  require(response.find("Access-Control-Allow-Headers: Content-Type") != std::string::npos,
          "upload preflight allows content type header");
}

}  // namespace

int main() {
  test_ping_route_returns_pong();
  test_upload_file_route_saves_raw_jpeg_body();
  test_upload_file_route_saves_multipart_mp4_file();
  test_upload_file_route_decodes_percent_encoded_utf8_filename();
  test_upload_file_worker_route_saves_raw_body();
  test_upload_file_worker_route_saves_multipart_body();
  test_upload_file_route_handles_cors_preflight();
  return 0;
}
