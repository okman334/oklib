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

## License

MIT. The design studies muduo's public architecture, but the implementation is
new code.
