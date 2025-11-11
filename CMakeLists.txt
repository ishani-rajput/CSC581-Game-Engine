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

# Shared engine components
set(ENGINE_COMMON
    ./src/entity.cpp
    ./src/input.cpp
    ./src/physics.cpp
    ./src/scaling.cpp
    ./src/timeline.cpp
    ./src/event_manager.cpp
)

# Single-player main (no networking)
add_executable(main
    ./src/main.cpp
    ${ENGINE_COMMON}
)

target_link_libraries(main
    PRIVATE SDL3::SDL3 SDL3_image::SDL3_image
)

# Multiplayer client
add_executable(client
    ./src/client.cpp
    ${ENGINE_COMMON}
    # object_model.h, registry.h are header-only
)

if (ZeroMQ_FOUND)
    target_link_libraries(client PRIVATE SDL3::SDL3 SDL3_image::SDL3_image libzmq)
else()
    target_link_libraries(client PRIVATE SDL3::SDL3 SDL3_image::SDL3_image ${ZMQ_LIB})
endif()

# Multiplayer server
add_executable(server
    ./src/server.cpp
    ./src/network_server.cpp
    ./src/event_manager.cpp
    ./src/timeline.cpp
)

if (ZeroMQ_FOUND)
    target_link_libraries(server PRIVATE SDL3::SDL3 ${ZMQ_LIB})
else()
    target_link_libraries(server PRIVATE SDL3::SDL3 ${ZMQ_LIB})
endif()

# Performance testing
add_executable(perf_main
    ./src/perf_main.cpp
    ./src/perf.cpp
    ./src/event_manager.cpp
    ./src/timeline.cpp
)

if (ZeroMQ_FOUND)
    target_link_libraries(perf_main PRIVATE SDL3::SDL3 ${ZMQ_LIB})
else()
    target_link_libraries(perf_main PRIVATE SDL3::SDL3 ${ZMQ_LIB})
endif()

# Section 5 server
add_executable(hybrid_server
    ./src/hybrid_server.cpp
    ./src/event_manager.cpp
    ./src/timeline.cpp
)

if (ZeroMQ_FOUND)
    target_link_libraries(hybrid_server PRIVATE SDL3::SDL3 ${ZMQ_LIB})
else()
    target_link_libraries(hybrid_server PRIVATE SDL3::SDL3 ${ZMQ_LIB})
endif()

# Section 5 client
add_executable(hybrid_client
    ./src/hybrid_client.cpp
    ./src/peer_manager.cpp
    ${ENGINE_COMMON}
)

if (ZeroMQ_FOUND)
    target_link_libraries(hybrid_client PRIVATE SDL3::SDL3 SDL3_image::SDL3_image libzmq)
else()
    target_link_libraries(hybrid_client PRIVATE SDL3::SDL3 SDL3_image::SDL3_image ${ZMQ_LIB})
endif()
