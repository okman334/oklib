# oklib

`oklib` is a C++20 networking library inspired by muduo's reactor architecture.
It uses modern standard-library facilities, avoids Boost, and targets Linux and
macOS.

## Goals

- `oklib::base`: small concurrency, time, and logging utilities.
- `oklib::net`: reactor core, TCP server/client, thread-safe send path.
- `oklib::http`: a compact HTTP server for validation and simple services.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the example HTTP server:

```sh
./build/examples/oklib_http_server 8080
curl http://127.0.0.1:8080/
```

HTTP handlers can also respond asynchronously from worker threads. This keeps
the `EventLoop` free while business code waits for a database or another HTTP
service:

```cpp
oklib::ThreadPool workers("http-workers");
workers.start(4);

server.set_async_http_callback(
    [&workers](oklib::http::HttpRequest request,
               oklib::http::HttpResponseWriter writer) {
      workers.run([request = std::move(request), writer] {
        auto response = writer.make_response();
        response.set_status_code(oklib::http::HttpStatusCode::ok);
        response.set_body("hello " + request.query());
        writer.send(std::move(response));
      });
    });
```

For large responses, stream HTTP/1.1 chunks instead of building one large body:

```cpp
workers.run([writer] {
  auto response = writer.make_response();
  response.set_status_code(oklib::http::HttpStatusCode::ok);
  response.set_content_type("text/plain");

  writer.start_chunked(std::move(response));
  writer.write_chunk("part one\n");
  writer.write_chunk("part two\n");
  writer.finish();
});
```

For large `Content-Length` or `Transfer-Encoding: chunked` request bodies, register
streaming body callbacks:

```cpp
server.set_streaming_http_callback(
    [](oklib::http::HttpRequest request,
       oklib::http::HttpRequestBodyStream body,
       oklib::http::HttpResponseWriter writer) {
      body.set_data_callback([](std::string_view chunk) {
        // Copy or process chunk before the callback returns.
      });
      body.set_complete_callback([writer] {
        auto response = writer.make_response();
        response.set_status_code(oklib::http::HttpStatusCode::ok);
        response.set_body("uploaded");
        writer.send(std::move(response));
      });
    });
```

HTTP servers can also observe TCP backpressure through the high-watermark
callback:

```cpp
server.set_high_water_mark_callback(
    [](const oklib::net::TcpConnectionPtr&, std::size_t buffered) {
      OKLIB_LOG_WARN << "http output buffered bytes=" << buffered;
    },
    1024 * 1024);
```

Run the TCP echo examples:

```sh
./build/examples/oklib_tcp_echo_server 9000 2
./build/examples/oklib_tcp_echo_client 127.0.0.1 9000 "hello"
```

Configure file logging:

```cpp
oklib::Logger::set_log_directory("logs");
oklib::Logger::set_file_basename("gateway");
oklib::Logger::set_flush_interval(std::chrono::seconds(3));
OKLIB_LOG_INFO << "server started";
oklib::Logger::flush();
```

By default, logs are written asynchronously to per-level files named from the
current program, such as `my_server.info.log` and `my_server.error.log`.
Buffered logs are flushed at least every 3 seconds by default, and the interval
can be changed with `Logger::set_flush_interval`.

## License

MIT. The design studies muduo's public architecture, but the implementation is
new code.
