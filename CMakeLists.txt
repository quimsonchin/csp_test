cmake_minimum_required(VERSION 3.16)
project(libscpDemo_csp C)

add_executable(sender_csp sender_csp.c)
target_link_libraries(sender_csp csp)

add_executable(receiver_csp receiver_csp.c)
target_link_libraries(receiver_csp csp)
