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

add_executable(oklib_timer_thread_test net/timer_thread_test.cpp)
target_link_libraries(oklib_timer_thread_test PRIVATE oklib::net)
add_test(NAME oklib.timer_thread COMMAND oklib_timer_thread_test)

if(OKLIB_ENABLE_TLS)
  add_executable(oklib_tcp_tls_test net/tcp_tls_test.cpp)
  target_link_libraries(oklib_tcp_tls_test PRIVATE oklib::net)
  add_test(NAME oklib.tcp.tls COMMAND oklib_tcp_tls_test)
endif()

add_executable(oklib_http_test http/http_test.cpp)
target_link_libraries(oklib_http_test PRIVATE oklib::http)
add_test(NAME oklib.http COMMAND oklib_http_test)

add_executable(oklib_http_client_test http/http_client_test.cpp)
target_link_libraries(oklib_http_client_test PRIVATE oklib::http)
add_test(NAME oklib.http.client COMMAND oklib_http_client_test)

add_executable(oklib_http_sync_client_test http/http_sync_client_test.cpp)
target_link_libraries(oklib_http_sync_client_test PRIVATE oklib::http)
add_test(NAME oklib.http.sync_client COMMAND oklib_http_sync_client_test)

add_executable(oklib_http_client_cache_test http/http_client_cache_test.cpp)
target_link_libraries(oklib_http_client_cache_test PRIVATE oklib::http)
add_test(NAME oklib.http.client_cache COMMAND oklib_http_client_cache_test)

add_executable(oklib_http_edge_test http/http_edge_test.cpp)
target_link_libraries(oklib_http_edge_test PRIVATE oklib::http)
add_test(NAME oklib.http.edge COMMAND oklib_http_edge_test)

add_executable(oklib_http_router_test http/http_router_test.cpp)
target_link_libraries(oklib_http_router_test PRIVATE oklib::http)
add_test(NAME oklib.http.router COMMAND oklib_http_router_test)

add_executable(oklib_http_content_type_test http/content_type_test.cpp)
target_link_libraries(oklib_http_content_type_test PRIVATE oklib::http)
add_test(NAME oklib.http.content_type COMMAND oklib_http_content_type_test)

if(OKLIB_ENABLE_TLS)
  add_executable(oklib_http_tls_test http/http_tls_test.cpp)
  target_link_libraries(oklib_http_tls_test PRIVATE oklib::http)
  add_test(NAME oklib.http.tls COMMAND oklib_http_tls_test)
endif()

add_executable(oklib_http_parser_test http/http_parser_test.cpp)
target_link_libraries(oklib_http_parser_test PRIVATE oklib::http)
add_test(NAME oklib.http.parser COMMAND oklib_http_parser_test)

add_executable(oklib_http_multipart_parser_test http/multipart_parser_test.cpp)
target_link_libraries(oklib_http_multipart_parser_test PRIVATE oklib::http)
add_test(NAME oklib.http.multipart_parser COMMAND oklib_http_multipart_parser_test)

add_executable(oklib_http_semantics_test http/http_semantics_test.cpp)
target_link_libraries(oklib_http_semantics_test PRIVATE oklib::http)
add_test(NAME oklib.http.semantics COMMAND oklib_http_semantics_test)

add_executable(oklib_http_cache_test http/http_cache_test.cpp)
target_link_libraries(oklib_http_cache_test PRIVATE oklib::http)
add_test(NAME oklib.http.cache COMMAND oklib_http_cache_test)

add_executable(oklib_websocket_core_test websocket/websocket_core_test.cpp)
target_link_libraries(oklib_websocket_core_test PRIVATE oklib::websocket)
add_test(NAME oklib.websocket.core COMMAND oklib_websocket_core_test)

add_executable(oklib_websocket_integration_test websocket/websocket_integration_test.cpp)
target_link_libraries(oklib_websocket_integration_test PRIVATE oklib::websocket)
add_test(NAME oklib.websocket.integration COMMAND oklib_websocket_integration_test)

add_test(
  NAME oklib.example.tcp_echo_server_logging
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/examples/verify_tcp_echo_server_logging.cmake)

add_executable(oklib_http_demo_routes_test examples/http_demo_routes_test.cpp)
target_include_directories(oklib_http_demo_routes_test PRIVATE ${PROJECT_SOURCE_DIR}/examples)
target_link_libraries(oklib_http_demo_routes_test PRIVATE oklib::http)
add_test(NAME oklib.example.http_demo_routes COMMAND oklib_http_demo_routes_test)
