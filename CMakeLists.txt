cmake_minimum_required(VERSION 3.10.2)

project(uptime_faker)

# Dobby
add_subdirectory(dobby)

# Uptime Faker
add_library(
    uptime_faker
    SHARED
    jni/main.cpp
)

# Header search paths
target_include_directories(
    uptime_faker
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/jni
    ${CMAKE_CURRENT_SOURCE_DIR}/dobby/include
)

# Libraries
target_link_libraries(
    uptime_faker
    PRIVATE
    dobby_static
    android
    log
)

# Output
set_target_properties(
    uptime_faker
    PROPERTIES
    LIBRARY_OUTPUT_NAME "uptime_faker"
)
