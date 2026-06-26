add_executable(oklib_http_server http_server.cpp)
target_link_libraries(oklib_http_server PRIVATE oklib::http)

add_executable(oklib_http_full_server http_full_server.cpp)
target_link_libraries(oklib_http_full_server PRIVATE oklib::http)

add_executable(oklib_http_client_demo http_client_demo.cpp)
target_link_libraries(oklib_http_client_demo PRIVATE oklib::http)

add_executable(oklib_http_sync_client_demo http_sync_client_demo.cpp)
target_link_libraries(oklib_http_sync_client_demo PRIVATE oklib::http)

add_executable(oklib_http_benchmark http_benchmark.cpp)
target_link_libraries(oklib_http_benchmark PRIVATE oklib::http)

add_executable(oklib_websocket_server websocket_server.cpp)
target_link_libraries(oklib_websocket_server PRIVATE oklib::websocket)

add_executable(oklib_websocket_client websocket_client.cpp)
target_link_libraries(oklib_websocket_client PRIVATE oklib::websocket)

if(OKLIB_ENABLE_TLS)
  add_executable(oklib_https_server https_server.cpp)
  target_link_libraries(oklib_https_server PRIVATE oklib::http)

  add_executable(oklib_tls_echo_server tls_echo_server.cpp)
  target_link_libraries(oklib_tls_echo_server PRIVATE oklib::net)

  add_executable(oklib_tls_echo_client tls_echo_client.cpp)
  target_link_libraries(oklib_tls_echo_client PRIVATE oklib::net)
endif()

add_executable(oklib_tcp_echo_server tcp_echo_server.cpp)
target_link_libraries(oklib_tcp_echo_server PRIVATE oklib::net)

add_executable(oklib_tcp_echo_client tcp_echo_client.cpp)
target_link_libraries(oklib_tcp_echo_client PRIVATE oklib::net)

add_executable(oklib_timer_thread_demo timer_thread_demo.cpp)
target_link_libraries(oklib_timer_thread_demo PRIVATE oklib::net)
