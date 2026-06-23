add_executable(oklib_http_server http_server.cpp)
target_link_libraries(oklib_http_server PRIVATE oklib::http)
