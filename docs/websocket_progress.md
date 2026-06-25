# WebSocket Progress

## Current Phase

- First WebSocket implementation complete.
- Next optional hardening: broader malformed-handshake tests and ping/pong/close edge assertions.

## Completed Work

- Added public target `oklib::websocket`.
- Added public headers under `include/oklib/websocket/`:
  - `WebSocketServer`
  - `WebSocketClient`
  - `WebSocketChannel`
  - `WebSocketService`
  - frame/handshake helpers and public enums/options.
- Implemented RFC 6455 handshake:
  - server validates `GET HTTP/1.1`, `Connection: Upgrade`, `Upgrade: websocket`,
    `Sec-WebSocket-Key`, and `Sec-WebSocket-Version: 13`;
  - internal SHA1 + Base64 for `Sec-WebSocket-Accept`;
  - client validates `101 Switching Protocols` and accept-key match.
- Implemented frame codec:
  - text/binary/continuation/close/ping/pong;
  - 125/126/127 payload lengths;
  - client masking and server unmasked output;
  - mask enforcement by endpoint role;
  - fragmentation reassembly;
  - control-frame and UTF-8 validation.
- Added `WebSocketServer` on top of `TcpServer`.
  - ordinary HTTP callback/router support remains available on the same port;
  - upgrade connections switch to WebSocket parsing;
  - WebSocket sends are thread-safe through `TcpConnection::send`.
- Added async `WebSocketClient`.
  - `open(url)`;
  - `on_open`, `on_message`, `on_close`, `on_error`;
  - `send_text`, `send_binary`, `send_ping`, `close`, `stop`;
  - `wss://` is wired through `OKLIB_ENABLE_TLS` when enabled.
- Added optional `OKLIB_ENABLE_WEBSOCKET_COMPRESSION`.
  - default OFF;
  - CMake uses `find_package(ZLIB REQUIRED)` only when ON;
  - first version negotiates `permessage-deflate` with no-context-takeover parameters.
- Added examples:
  - `oklib_websocket_server`
  - `oklib_websocket_client`
- Added tests:
  - RFC accept-key;
  - frame encode/decode, extended length, mask enforcement, fragmentation, control-frame rejection,
    invalid UTF-8;
  - server/client WebSocket echo;
  - ordinary HTTP `/ping` and WebSocket Upgrade sharing one server port.
  - `wss://` local round trip under `OKLIB_ENABLE_TLS=ON`;
  - `permessage-deflate` negotiation and compressed echo under
    `OKLIB_ENABLE_WEBSOCKET_COMPRESSION=ON`.

## Incomplete / Follow-Up Work

- Add deeper malformed-handshake integration tests for 400/426 paths.
- Add explicit ping/pong and close-handshake integration assertions.
- Consider a future blocking WebSocket client wrapper on top of the async client.

## Current Blockers

- None.

## Latest Test Result

- 2026-06-26: `cmake --build build --parallel` passed after adding WebSocket server/client/examples.
- 2026-06-26: `ctest --test-dir build --output-on-failure` passed, 18/18 tests.
- 2026-06-26: `cmake --build build-tls --parallel` passed.
- 2026-06-26: `ctest --test-dir build-tls --output-on-failure` passed, 19/19 tests,
  including local `wss://` WebSocket round trip.
- 2026-06-26: `cmake -S . -B build-ws-zlib -DCMAKE_BUILD_TYPE=Debug -DOKLIB_ENABLE_WEBSOCKET_COMPRESSION=ON` passed.
- 2026-06-26: `cmake --build build-ws-zlib --parallel` passed.
- 2026-06-26: `ctest --test-dir build-ws-zlib --output-on-failure` passed, 18/18 tests,
  including `permessage-deflate` negotiation and compressed echo.

## Latest Commit / Push

- Pending.

## Next Step

- Commit/push this branch, then decide whether to merge to `main` or add malformed-handshake hardening first.
