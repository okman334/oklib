cmake_minimum_required(VERSION 3.20)

project(oklib
  VERSION 0.1.0
  DESCRIPTION "A modern C++20 reactor-based networking library"
  LANGUAGES CXX)

option(OKLIB_BUILD_TESTS "Build oklib tests" ON)
option(OKLIB_BUILD_EXAMPLES "Build oklib examples" ON)
option(OKLIB_ENABLE_TLS "Enable optional OpenSSL TLS support" OFF)
option(OKLIB_ENABLE_WEBSOCKET_COMPRESSION "Enable optional WebSocket permessage-deflate support" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)
if(OKLIB_ENABLE_TLS)
  find_package(OpenSSL REQUIRED)
endif()
if(OKLIB_ENABLE_WEBSOCKET_COMPRESSION)
  find_package(ZLIB REQUIRED)
endif()

add_library(oklib_base STATIC
  src/base/countdown_latch.cpp
  src/base/log_stream.cpp
  src/base/logging.cpp
  src/base/thread_pool.cpp
  src/base/timestamp.cpp)
add_library(oklib::base ALIAS oklib_base)
target_include_directories(oklib_base
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(oklib_base PUBLIC cxx_std_20)
target_link_libraries(oklib_base PUBLIC Threads::Threads)

add_library(oklib_net STATIC
  src/net/acceptor.cpp
  src/net/buffer.cpp
  src/net/callbacks.cpp
  src/net/channel.cpp
  src/net/connector.cpp
  src/net/event_loop.cpp
  src/net/event_loop_thread.cpp
  src/net/event_loop_thread_pool.cpp
  src/net/inet_address.cpp
  src/net/poller.cpp
  src/net/socket.cpp
  src/net/tcp_client.cpp
  src/net/tcp_connection.cpp
  src/net/tcp_server.cpp
  src/net/timer_thread.cpp)
if(OKLIB_ENABLE_TLS)
  target_sources(oklib_net PRIVATE src/net/tls_engine.cpp)
endif()
add_library(oklib::net ALIAS oklib_net)
target_link_libraries(oklib_net PUBLIC oklib::base)
if(OKLIB_ENABLE_TLS)
  target_link_libraries(oklib_net PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(oklib_net
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(oklib_net PUBLIC cxx_std_20)
target_compile_definitions(oklib_net PUBLIC OKLIB_ENABLE_TLS=$<BOOL:${OKLIB_ENABLE_TLS}>)

add_library(oklib_http STATIC
  src/http/content_type.cpp
  src/http/http_cache.cpp
  src/http/http_client.cpp
  src/http/http_client_cache.cpp
  src/http/http_context.cpp
  src/http/http_headers.cpp
  src/http/http_parser.cpp
  src/http/http_request_body_stream.cpp
  src/http/http_request.cpp
  src/http/http_response.cpp
  src/http/http_response_writer.cpp
  src/http/http_router.cpp
  src/http/http_semantics.cpp
  src/http/http_server.cpp
  src/http/http_sync_client.cpp
  src/http/multipart_parser.cpp
  src/http/upload_file_writer.cpp
  src/http/url_encoding.cpp)
add_library(oklib::http ALIAS oklib_http)
target_link_libraries(oklib_http PUBLIC oklib::net)
target_include_directories(oklib_http
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(oklib_http PUBLIC cxx_std_20)
target_compile_definitions(oklib_http PUBLIC OKLIB_ENABLE_TLS=$<BOOL:${OKLIB_ENABLE_TLS}>)

add_library(oklib_websocket STATIC
  src/websocket/websocket_channel.cpp
  src/websocket/websocket_client.cpp
  src/websocket/websocket_compression.cpp
  src/websocket/websocket_frame.cpp
  src/websocket/websocket_handshake.cpp
  src/websocket/websocket_server.cpp)
add_library(oklib::websocket ALIAS oklib_websocket)
target_link_libraries(oklib_websocket PUBLIC oklib::http)
if(OKLIB_ENABLE_WEBSOCKET_COMPRESSION)
  target_link_libraries(oklib_websocket PUBLIC ZLIB::ZLIB)
endif()
target_include_directories(oklib_websocket
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(oklib_websocket PUBLIC cxx_std_20)
target_compile_definitions(oklib_websocket
  PUBLIC
    OKLIB_ENABLE_TLS=$<BOOL:${OKLIB_ENABLE_TLS}>
    OKLIB_ENABLE_WEBSOCKET_COMPRESSION=$<BOOL:${OKLIB_ENABLE_WEBSOCKET_COMPRESSION}>)

if(OKLIB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(OKLIB_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
