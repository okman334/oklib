set(example_file "${PROJECT_SOURCE_DIR}/examples/tcp_echo_server.cpp")
file(READ "${example_file}" example_source)

if(NOT example_source MATCHES "oklib/base/logging\\.h")
  message(FATAL_ERROR "tcp_echo_server.cpp must include oklib/base/logging.h")
endif()

if(NOT example_source MATCHES "OKLIB_LOG_INFO")
  message(FATAL_ERROR "tcp_echo_server.cpp must call OKLIB_LOG_INFO")
endif()
