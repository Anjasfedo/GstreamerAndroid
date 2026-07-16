cmake_minimum_required(VERSION 3.22.1)
project("tutorial-1")

add_library(
        tutorial-1
        SHARED
        tutorial-1.c)

find_library(
        log-lib
        log)

target_link_libraries(
        tutorial-1
        ${log-lib})