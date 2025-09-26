cmake_minimum_required(VERSION 3.10)
project(FeelingLoopy)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL3 REQUIRED CONFIG)
find_package(SDL3_image REQUIRED CONFIG)

# ZeroMQ (clients and server)
find_package(ZeroMQ QUIET)
if (NOT ZeroMQ_FOUND)
    find_library(ZMQ_LIB zmq REQUIRED)
endif()

include_directories(include)

# Main engine executable
add_executable(main
    ./src/main.cpp
    ./src/entity.cpp
    ./src/input.cpp
    ./src/physics.cpp
    ./src/scaling.cpp
    ./src/timeline.cpp
)

target_link_libraries(main
    PRIVATE
        SDL3::SDL3
        SDL3_image::SDL3_image
)

# Client executable
add_executable(client
    ./src/client.cpp
    ./src/entity.cpp
    ./src/input.cpp
    ./src/physics.cpp
    ./src/scaling.cpp
    ./src/timeline.cpp
)

if (ZeroMQ_FOUND)
    target_link_libraries(client PRIVATE SDL3::SDL3 SDL3_image::SDL3_image libzmq)
else()
    target_link_libraries(client PRIVATE SDL3::SDL3 SDL3_image::SDL3_image ${ZMQ_LIB})
endif()

# Headless server executable
add_executable(server
    ./src/server.cpp
    ./src/physics.cpp
    ./src/timeline.cpp
    ./src/network_server.cpp 
)

if (ZeroMQ_FOUND)
    target_link_libraries(server PRIVATE SDL3::SDL3 libzmq)
else()
    target_link_libraries(server PRIVATE SDL3::SDL3 ${ZMQ_LIB})
endif()
