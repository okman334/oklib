# WebSocket Progress

## Current Phase

- First WebSocket implementation complete.
- Hardening pass 1 complete.
- Next optional hardening: subprotocol edge cases, malformed client 101 responses, and stress/fuzz-style frame sequences.

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
- Hardening pass 1:
  - malformed server handshakes return 400 or 426 with `Sec-WebSocket-Version: 13`;
  - server automatically replies pong to client ping;
  - client automatically replies masked pong to server ping;
  - normal close frames preserve code/reason in `on_close`;
  - unmasked client frames close with 1002 and notify server `on_close`;
  - masked server frames close with 1002 and notify client `on_close`.

## Incomplete / Follow-Up Work

- Add malformed client response tests, such as bad `Sec-WebSocket-Accept`.
- Add subprotocol selector/response edge tests.
- Add fragmented control-frame and RSV integration tests beyond codec-only coverage.
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
- 2026-06-26: `oklib.websocket.integration` passed after hardening pass 1.
- 2026-06-26: `cmake --build build --parallel` passed after hardening pass 1.
- 2026-06-26: `ctest --test-dir build --output-on-failure` passed, 18/18 tests after hardening pass 1.
- 2026-06-26: `cmake --build build-tls --parallel` passed after hardening pass 1.
- 2026-06-26: `ctest --test-dir build-tls --output-on-failure` passed, 19/19 tests after hardening pass 1.
- 2026-06-26: `cmake --build build-ws-zlib --parallel` passed after hardening pass 1.
- 2026-06-26: `ctest --test-dir build-ws-zlib --output-on-failure` passed, 18/18 tests after hardening pass 1.

## Latest Commit / Push

- Implementation commit: `59109c7`.
- Push: `origin/codex/websocket-standard`.

## Next Step

- Decide whether to merge to `main`, or continue with subprotocol/malformed-client-response tests.
