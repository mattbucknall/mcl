cmake_minimum_required(VERSION 3.25)
project(mcl C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS_DEBUG "-DMCL_DEBUG -g -Og")

add_executable(test-mcl test-mcl.c)

enable_testing()
add_test(NAME test-mcl COMMAND $<TARGET_FILE:test-mcl>)
