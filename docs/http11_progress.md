# HTTP/1.1 Progress

## Current Phase

- Next Phase D: complete HTTP examples and benchmark.
  - Next scope: full HTTP server/client examples, HTTPS examples gated by `OKLIB_ENABLE_TLS`, and local keep-alive benchmark.

## Completed Phases

- Phase 0: progress tracking and engineering setup.
  - Added `docs/http11_progress.md`.
  - Commit: `b9e09eb`.
  - Push: `origin/codex/http11-compliance`.
- Baseline verification on branch `codex/http11-compliance`.
  - `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`: passed.
  - `cmake --build build --parallel`: passed.
  - `ctest --test-dir build --output-on-failure`: passed, 7/7 tests.
- Phase 1: HTTP core model and incremental parser.
  - Added `HttpHeaders` with case-insensitive lookup and repeated field preservation.
  - Extended `HttpRequest` with method token, request-target form, body, content length, and trailers.
  - Added `HttpParser` for incremental request/response parsing, fixed-length body, chunked body, trailers, pipeline leftover, and strict ambiguous framing rejection.
  - Added `oklib.http.parser` tests.
  - Commit: `5e44d7b`.
  - Push: `origin/codex/http11-compliance`.
- Phase 2: buffered `HttpServer` upgrade.
  - Routed `HttpContext` through `HttpParser`.
  - `HttpServer` now supports buffered Content-Length request bodies, chunked request bodies, trailers, Host validation, ambiguous framing rejection, and multiple pipelined requests in one read buffer.
  - Added integration coverage in `oklib.http`.
  - Commit: `eb42d8c`.
  - Push: `origin/codex/http11-compliance`.
- Phase 3: server streaming and backpressure.
  - Added async buffered response callback and `HttpResponseWriter`.
  - Added chunked response streaming.
  - Added Content-Length request body streaming callbacks.
  - Added chunked request body streaming callbacks with trailer consumption.
  - Exposed HTTP/TCP high-watermark callback wiring and added slow-client backpressure coverage.
  - Commits: `69c37d6`, `710a4cb`, `d8b6cb5`, `add304b`.
  - Push: `origin/codex/http11-compliance`.
- Phase 4: HTTP client.
  - Added nonblocking buffered `HttpClient`, request serialization, response parser integration, keep-alive reuse, server-close reconnect for later requests, queued-before-connect request coalescing, optional TCP retry wiring, and chunked response decoding.
  - Added streaming response callbacks for fixed-length and chunked responses.
  - Added `Expect: 100-continue` flow that sends headers first, waits for interim `100 Continue`, hides interim 100 responses from user callbacks, then sends the request body.
  - Commits: `bb1ff92`, `6d47852`.
  - Push: `origin/codex/http11-compliance`.
- Phase 5: RFC 9110 semantic helpers.
  - Added standard method metadata helpers and status reason phrase/status classification helpers.
  - Added `HttpResponse::set_status_code(int)` for unknown or extension status codes while preserving custom reason phrases.
  - Added Range and Content-Range parsing helpers.
  - Added Entity-Tag, If-Match, If-None-Match, IMF-fixdate, If-Modified-Since, and If-Unmodified-Since helpers.
  - Added `HttpServer::set_allowed_methods(...)` and automatic `OPTIONS *` handling with `Allow`.
  - Verified CONNECT, TRACE, and Upgrade requests/headers are exposed to application callbacks by default.
  - Commit: `a89114f`.
  - Push: `origin/codex/http11-compliance`.
- Phase 6: RFC 9111 cache-related helpers.
  - Added `HttpCache` helper APIs for Cache-Control directive parsing, quoted values, and delta-seconds.
  - Added Age and Expires parsing helpers.
  - Added freshness helpers for current age, explicit freshness lifetime, private/shared cache behavior, `no-cache`, `no-store`, and `private`.
  - Added Vary parsing and request-header matching helpers.
  - Added validator header generation from cached ETag and Last-Modified metadata.
  - Commit: `3e5cdc2`.
  - Push: `origin/codex/http11-compliance`.
- Phase A: stricter RFC boundary hardening.
  - Added `oklib.http.edge` coverage for 204/304 no-body parsing, invalid chunk extensions, forbidden trailer framing fields, HEAD client responses, and comma-separated `Connection` tokens.
  - Parser now ignores prohibited response bodies for 1xx/204/205/304, validates chunk extensions, and rejects forbidden `Content-Length`/`Transfer-Encoding`/`Host` trailer fields.
  - Server response serialization suppresses bodies for no-body status codes.
  - Server and client now parse `Connection` as a comma-separated token list.
  - Commit: `c744b4c`.
  - Push: `origin/codex/http11-compliance`.
- Phase B: `HttpClient` private in-process cache.
  - Added opt-in `HttpClientCache` with LRU entry/body limits.
  - Added `HttpClientOptions::cache` and `HttpClientResponseSource` to mark network/cache/revalidated responses.
  - Buffered GET/HEAD now supports fresh cache hits, stale ETag/Last-Modified revalidation, `304` body reuse, Vary matching, `no-store`, and body-size exclusion.
  - Added `oklib.http.client_cache` integration coverage.
  - Commit: `798565e`.
  - Push: `origin/codex/http11-compliance`.
- Phase C: optional OpenSSL TLS client/server.
  - Added `OKLIB_ENABLE_TLS` CMake option with optional `find_package(OpenSSL REQUIRED)` and OpenSSL link targets.
  - Added public `TlsClientOptions` and `TlsServerOptions`; `HttpClientOptions` and `HttpServer` now expose optional TLS configuration.
  - Added OpenSSL memory-BIO TLS engine and TCP send filtering/raw send support so existing HTTP callbacks and response writers keep the same API.
  - Added gated `oklib.http.tls` tests for HTTPS client/server round trip and self-signed certificate verification failure.
  - Stabilized the client cache body-limit test by separating independent EventLoop fixtures.
  - Commits: `b96c044`, `7a7f951`.
  - Push: `origin/codex/http11-compliance`.

## Incomplete Phases

- Phase D: complete HTTP examples and benchmark.

## Current Blockers

- None.

## Latest Test Result

- 2026-06-24: Debug build passed.
- 2026-06-24: `ctest --test-dir build --output-on-failure` passed, 8/8 tests.
- 2026-06-25: `cmake --build build --parallel` passed.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 8/8 tests.
- 2026-06-25: `cmake --build build --parallel` passed after chunked response streaming.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 8/8 tests after chunked response streaming.
- 2026-06-25: `oklib.http` Content-Length request body streaming integration test passed locally.
- 2026-06-25: `cmake --build build --parallel` passed after Content-Length request streaming.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 8/8 tests after Content-Length request streaming and chunked streaming rejection guard.
- 2026-06-25: `cmake --build build --parallel` passed after completing Phase 3.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 8/8 tests after completing Phase 3.
- 2026-06-25: `oklib.http.client` passed after buffered HttpClient implementation.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 9/9 tests after buffered HttpClient implementation.
- 2026-06-25: `oklib.http.client` passed after streaming response callbacks and `Expect: 100-continue`.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 9/9 tests after completing Phase 4.
- 2026-06-25: `cmake --build build --parallel` passed after completing Phase 5.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 10/10 tests after completing Phase 5.
- 2026-06-25: `cmake --build build --parallel` passed after completing Phase 6.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 11/11 tests after completing Phase 6.
- 2026-06-25: `cmake --build build --parallel` passed after Phase A boundary hardening.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 12/12 tests after Phase A boundary hardening.
- 2026-06-25: `cmake --build build --parallel` passed after Phase B private cache.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 13/13 tests after Phase B private cache.
- 2026-06-25: `cmake --build build --parallel` passed after Phase C TLS changes.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 13/13 tests after Phase C TLS changes.
- 2026-06-25: `cmake -S . -B build-tls -DOKLIB_ENABLE_TLS=ON -DCMAKE_BUILD_TYPE=Debug` passed.
- 2026-06-25: `cmake --build build-tls --parallel` passed.
- 2026-06-25: `ctest --test-dir build-tls --output-on-failure` passed, 14/14 tests after Phase C TLS changes.

## Latest Commit / Push

- Phase 0 commit pushed: `b9e09eb`.
- Phase 1 commit pushed: `5e44d7b`.
- Phase 2 commit pushed: `eb42d8c`.
- Phase 3 async response commit: `69c37d6`.
- Phase 3 chunked response streaming commit: `710a4cb`.
- Phase 3 request body streaming commit: `d8b6cb5`.
- Phase 3 completion commit: `add304b`.
- Phase 4 buffered HttpClient commit: `bb1ff92`.
- Phase 4 completion commit: `6d47852`.
- Phase 5 semantic helpers commit: `a89114f`.
- Phase 6 cache helpers commit: `3e5cdc2`.
- Phase A RFC boundary hardening commit: `c744b4c`.
- Phase B HttpClient private cache commit: `798565e`.
- Phase B cache test stabilization commit: `b96c044`.
- Phase C optional TLS commit: `7a7f951`.
- Push: `origin/codex/http11-compliance`.

## Next Step

- Start Phase D with full HTTP server/client examples, HTTPS examples gated by `OKLIB_ENABLE_TLS`, and a local keep-alive benchmark.
