add_executable(oklib_http_server http_server.cpp)
target_link_libraries(oklib_http_server PRIVATE oklib::http)

add_executable(oklib_tcp_echo_server tcp_echo_server.cpp)
target_link_libraries(oklib_tcp_echo_server PRIVATE oklib::net)

add_executable(oklib_tcp_echo_client tcp_echo_client.cpp)
target_link_libraries(oklib_tcp_echo_client PRIVATE oklib::net)
