# oklib Implementation Plan

This repository is implemented in phases:

1. Project scaffold, CI, license, and CMake/CTest wiring.
2. `oklib::base` standard-library utilities.
3. `oklib::net` reactor core with epoll on Linux and kqueue on macOS.
4. TCP server/client with thread-safe `TcpConnection::send`.
5. Basic HTTP server and examples.
