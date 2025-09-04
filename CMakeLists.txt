cmake_minimum_required(VERSION 3.10)
project(FeelingLoopy)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL3 REQUIRED CONFIG)
find_package(SDL3_image REQUIRED CONFIG)

include_directories(include)
add_executable(main
    ./src/main.cpp
    ./src/entity.cpp
    ./src/input.cpp
    ./src/physics.cpp
    ./src/scaling.cpp
)

target_link_libraries(main
    PRIVATE
        SDL3::SDL3
        SDL3_image::SDL3_image
)