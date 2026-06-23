add_executable(oklib_smoke_test smoke_test.cpp)
target_link_libraries(oklib_smoke_test PRIVATE oklib::base)
add_test(NAME oklib.smoke COMMAND oklib_smoke_test)
