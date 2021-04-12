cmake_minimum_required(VERSION 3.5.1)

project(bus)
enable_testing()

find_package(Protobuf REQUIRED)
include_directories(${Protobuf_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

protobuf_generate_cpp(LIB_PROTO_SRCS LIB_PROTO_HDRS service.proto)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS messages.proto)

add_library(bus
    bus.h bus.cpp
    proto_bus.h proto_bus.cpp
    connect_pool.h connect_pool.cpp
    endpoint_manager.h endpoint_manager.cpp
    error.h error.cpp
    util.h util.cpp
    ${LIB_PROTO_HDRS} ${LIB_PROTO_SRCS})

# old cmake
find_package(Threads::Threads QUIET)
target_link_libraries(bus PUBLIC Threads::Threads)

add_executable(testBus testBus.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS})

target_link_libraries(testBus ${Protobuf_LIBRARIES} bus)

add_executable(testService testService.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS})

set_property(TARGET testService PROPERTY CXX_STANDARD 17)
target_link_libraries(testService ${Protobuf_LIBRARIES} bus)

add_executable(benchProxy benchProxy.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS})

target_link_libraries(benchProxy ${Protobuf_LIBRARIES} bus)

add_test(NAME bus COMMAND testBus)
add_test(NAME service COMMAND testService)
