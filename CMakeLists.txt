cmake_minimum_required(VERSION 3.10.2)

project(uptime_faker)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(dobby)

add_library(uptime_faker SHARED
    jni/main.cpp
)

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(uptime_faker
    ${log-lib}
    ${android-lib}
    dobby_static
)
