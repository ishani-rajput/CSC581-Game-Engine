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

# ---------------------------
# EVENT MANAGER (Milestone 4)
# ---------------------------
add_library(engine_events
    ./src/event_manager.cpp
    ./src/timeline.cpp        # <-- ADDED: event_manager depends on Timeline
)
target_include_directories(engine_events PUBLIC include)  # Changed to PUBLIC
target_link_libraries(engine_events PUBLIC SDL3::SDL3)    # Changed to PUBLIC

# ======================================================
#  EXECUTABLES
# ======================================================

# ---------------------------
# MAIN GAME (Milestone 1–2)
# ---------------------------
add_executable(main
    ./src/main.cpp
    ./src/entity.cpp
    ./src/input.cpp
    ./src/physics.cpp
    ./src/scaling.cpp
    # ./src/timeline.cpp  <-- REMOVED: now in engine_events library
)

target_link_libraries(main
    PRIVATE
        SDL3::SDL3
        SDL3_image::SDL3_image
        engine_events
)

# ---------------------------
# CLIENT (Milestone 3)
# ---------------------------
add_executable(client
    ./src/client.cpp
    ./src/entity.cpp
    ./src/input.cpp
    ./src/physics.cpp
    ./src/scaling.cpp
    # ./src/timeline.cpp  <-- REMOVED: now in engine_events library
    ./src/peer_manager.cpp
)

if (ZeroMQ_FOUND)
    target_link_libraries(client PRIVATE
        SDL3::SDL3
        SDL3_image::SDL3_image
        libzmq
        engine_events
    )
else()
    target_link_libraries(client PRIVATE
        SDL3::SDL3
        SDL3_image::SDL3_image
        ${ZMQ_LIB}
        engine_events
    )
endif()

# ---------------------------
# SERVER (Headless)
# ---------------------------
add_executable(server
    ./src/server.cpp
    ./src/network_server.cpp
)

if (ZeroMQ_FOUND)
    target_link_libraries(server PRIVATE
        SDL3::SDL3
        libzmq
        engine_events
    )
else()
    target_link_libraries(server PRIVATE
        SDL3::SDL3
        ${ZMQ_LIB}
        engine_events
    )
endif()

# ---------------------------
# PERFORMANCE TEST (Part 1C)
# ---------------------------
add_executable(perf
    ./src/perf_main.cpp
    ./src/perf.cpp
)

if (ZeroMQ_FOUND)
    target_link_libraries(perf PRIVATE libzmq)
else()
    target_link_libraries(perf PRIVATE ${ZMQ_LIB})
endif()
