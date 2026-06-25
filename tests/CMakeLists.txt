add_executable(oklib_smoke_test smoke_test.cpp)
target_link_libraries(oklib_smoke_test PRIVATE oklib::base)
add_test(NAME oklib.smoke COMMAND oklib_smoke_test)

add_executable(oklib_base_test base/base_test.cpp)
target_link_libraries(oklib_base_test PRIVATE oklib::base)
add_test(NAME oklib.base COMMAND oklib_base_test)

add_test(
  NAME oklib.layout.base_sources
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/base/verify_base_sources.cmake)

add_executable(oklib_reactor_test net/reactor_test.cpp)
target_link_libraries(oklib_reactor_test PRIVATE oklib::net)
add_test(NAME oklib.reactor COMMAND oklib_reactor_test)

add_executable(oklib_tcp_test net/tcp_test.cpp)
target_link_libraries(oklib_tcp_test PRIVATE oklib::net)
add_test(NAME oklib.tcp COMMAND oklib_tcp_test)

add_executable(oklib_http_test http/http_test.cpp)
target_link_libraries(oklib_http_test PRIVATE oklib::http)
add_test(NAME oklib.http COMMAND oklib_http_test)

add_executable(oklib_http_client_test http/http_client_test.cpp)
target_link_libraries(oklib_http_client_test PRIVATE oklib::http)
add_test(NAME oklib.http.client COMMAND oklib_http_client_test)

add_executable(oklib_http_parser_test http/http_parser_test.cpp)
target_link_libraries(oklib_http_parser_test PRIVATE oklib::http)
add_test(NAME oklib.http.parser COMMAND oklib_http_parser_test)

add_executable(oklib_http_semantics_test http/http_semantics_test.cpp)
target_link_libraries(oklib_http_semantics_test PRIVATE oklib::http)
add_test(NAME oklib.http.semantics COMMAND oklib_http_semantics_test)

add_executable(oklib_http_cache_test http/http_cache_test.cpp)
target_link_libraries(oklib_http_cache_test PRIVATE oklib::http)
add_test(NAME oklib.http.cache COMMAND oklib_http_cache_test)

add_test(
  NAME oklib.example.tcp_echo_server_logging
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/examples/verify_tcp_echo_server_logging.cmake)
