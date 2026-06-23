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

Run the TCP echo examples:

```sh
./build/examples/oklib_tcp_echo_server 9000 2
./build/examples/oklib_tcp_echo_client 127.0.0.1 9000 "hello"
```

Configure file logging:

```cpp
oklib::Logger::set_log_directory("logs");
oklib::Logger::set_file_basename("gateway");
OKLIB_LOG_INFO << "server started";
oklib::Logger::flush();
```

By default, logs are written asynchronously to per-level files named from the
current program, such as `my_server.info.log` and `my_server.error.log`.

## License

MIT. The design studies muduo's public architecture, but the implementation is
new code.
