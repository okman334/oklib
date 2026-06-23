add_executable(oklib_smoke_test smoke_test.cpp)
target_link_libraries(oklib_smoke_test PRIVATE oklib::base)
add_test(NAME oklib.smoke COMMAND oklib_smoke_test)

add_executable(oklib_base_test base/base_test.cpp)
target_link_libraries(oklib_base_test PRIVATE oklib::base)
add_test(NAME oklib.base COMMAND oklib_base_test)

add_executable(oklib_reactor_test net/reactor_test.cpp)
target_link_libraries(oklib_reactor_test PRIVATE oklib::net)
add_test(NAME oklib.reactor COMMAND oklib_reactor_test)

add_executable(oklib_tcp_test net/tcp_test.cpp)
target_link_libraries(oklib_tcp_test PRIVATE oklib::net)
add_test(NAME oklib.tcp COMMAND oklib_tcp_test)

add_executable(oklib_http_test http/http_test.cpp)
target_link_libraries(oklib_http_test PRIVATE oklib::http)
add_test(NAME oklib.http COMMAND oklib_http_test)
