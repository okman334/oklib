add_executable(oklib_smoke_test smoke_test.cpp)
target_link_libraries(oklib_smoke_test PRIVATE oklib::base)
add_test(NAME oklib.smoke COMMAND oklib_smoke_test)

add_executable(oklib_base_test base/base_test.cpp)
target_link_libraries(oklib_base_test PRIVATE oklib::base)
add_test(NAME oklib.base COMMAND oklib_base_test)
