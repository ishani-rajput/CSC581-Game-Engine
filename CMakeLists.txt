cmake_minimum_required(VERSION 3.10)
project(FeelingLoopy)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL3 REQUIRED CONFIG)
find_package(SDL3_image REQUIRED CONFIG)

find_package(ZeroMQ QUIET)
if (ZeroMQ_FOUND)
    if (TARGET libzmq)
        set(ZMQ_LIB libzmq)
    elseif (TARGET ZeroMQ::libzmq)
        set(ZMQ_LIB ZeroMQ::libzmq)
    elseif (TARGET ZeroMQ::ZeroMQ)
        set(ZMQ_LIB ZeroMQ::ZeroMQ)
    endif()
endif()
if (NOT ZMQ_LIB)
    find_library(ZMQ_LIB zmq REQUIRED)
endif()

include_directories(include)

add_library(engine_objs
    src/entity.cpp
    src/input.cpp
    src/physics.cpp
    src/scaling.cpp
    src/timeline.cpp
    src/network_server.cpp
    src/peer_manager.cpp
    src/perf.cpp  
    src/event_manager.cpp        
)

target_link_libraries(engine_objs
    PRIVATE
        SDL3::SDL3
        SDL3_image::SDL3_image
        ${ZMQ_LIB}
)
target_include_directories(engine_objs PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_executable(main
    src/main.cpp
)
target_link_libraries(main
    PRIVATE
        engine_objs
        SDL3::SDL3
        SDL3_image::SDL3_image
)

add_executable(client
    src/client.cpp
)
target_link_libraries(client
    PRIVATE
        engine_objs
        SDL3::SDL3
        SDL3_image::SDL3_image
        ${ZMQ_LIB}
)

add_executable(server
    src/server.cpp
)
target_link_libraries(server
    PRIVATE
        engine_objs
        SDL3::SDL3   
        ${ZMQ_LIB}
)

add_executable(perf_runner
    src/perf_main.cpp
)
target_link_libraries(perf_runner
    PRIVATE
        engine_objs
        ${ZMQ_LIB}
)

add_executable(bubble_shooter
    src/bubble_shooter.cpp
)
target_link_libraries(bubble_shooter
    PRIVATE
        engine_objs
        SDL3::SDL3
        SDL3_image::SDL3_image
)

add_executable(snake
    src/snake.cpp
)
target_link_libraries(snake
    PRIVATE
        engine_objs
        SDL3::SDL3
        SDL3_image::SDL3_image
)

if (MSVC)
    target_compile_options(engine_objs PRIVATE /W4)
else()
    target_compile_options(engine_objs PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(engine_objs PROPERTIES FOLDER "engine")
set_target_properties(main client server perf_runner bubble_shooter snake PROPERTIES FOLDER "apps")

message(STATUS "SDL3 include: ${SDL3_DIR}")
message(STATUS "ZeroMQ lib: ${ZMQ_LIB}")