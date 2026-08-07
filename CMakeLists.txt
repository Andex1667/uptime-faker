cmake_minimum_required(VERSION 3.10.2)
project(uptime_faker)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(uptime_faker SHARED jni/main.cpp)

target_link_libraries(uptime_faker
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/jni/libdobby.a
    log
    android
)
