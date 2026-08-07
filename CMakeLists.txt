cmake_minimum_required(VERSION 3.10.2)
project(uptime_faker)

# Dobby 소스 경로 추가
add_subdirectory(dobby)

# 공유 라이브러리 빌드 설정
add_library(uptime_faker SHARED jni/main.cpp)

# Dobby 라이브러리 및 안드로이드 라이브러리 링크
target_link_libraries(uptime_faker PRIVATE dobby_static android log)

# 빌드 옵션
set_target_properties(uptime_faker PROPERTIES LIBRARY_OUTPUT_NAME "uptime_faker")
