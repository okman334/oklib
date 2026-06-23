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

add_library(oklib_base INTERFACE)
add_library(oklib::base ALIAS oklib_base)
target_include_directories(oklib_base
  INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(oklib_base INTERFACE cxx_std_20)

add_library(oklib_net INTERFACE)
add_library(oklib::net ALIAS oklib_net)
target_link_libraries(oklib_net INTERFACE oklib::base)
target_include_directories(oklib_net
  INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(oklib_net INTERFACE cxx_std_20)

add_library(oklib_http INTERFACE)
add_library(oklib::http ALIAS oklib_http)
target_link_libraries(oklib_http INTERFACE oklib::net)
target_include_directories(oklib_http
  INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(oklib_http INTERFACE cxx_std_20)

if(OKLIB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(OKLIB_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
