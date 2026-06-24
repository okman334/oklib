cmake_minimum_required(VERSION 3.20)

project(oklib
  VERSION 0.1.0
  DESCRIPTION "A modern C++20 reactor-based networking library"
  LANGUAGES CXX)

option(OKLIB_BUILD_TESTS "Build oklib tests" ON)
option(OKLIB_BUILD_EXAMPLES "Build oklib examples" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
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
  src/net/tcp_server.cpp)
add_library(oklib::net ALIAS oklib_net)
target_link_libraries(oklib_net PUBLIC oklib::base)
target_include_directories(oklib_net
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(oklib_net PUBLIC cxx_std_20)

add_library(oklib_http STATIC
  src/http/http_context.cpp
  src/http/http_request.cpp
  src/http/http_response.cpp
  src/http/http_server.cpp)
add_library(oklib::http ALIAS oklib_http)
target_link_libraries(oklib_http PUBLIC oklib::net)
target_include_directories(oklib_http
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(oklib_http PUBLIC cxx_std_20)

if(OKLIB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(OKLIB_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
