cmake_minimum_required(VERSION 3.16)
project(hello_csp C)

include_directories("$ENV{HOME}/libcsp/include")
link_directories("$ENV{HOME}/libcsp/build")

add_executable(hello_csp hello_csp.c)
target_link_libraries(hello_csp csp)
