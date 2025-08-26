cmake_minimum_required(VERSION 3.10)
project(FeelingLoopy)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL3 REQUIRED)

add_executable(main ./src/main.cpp)

target_link_libraries(main PRIVATE SDL3::SDL3)
