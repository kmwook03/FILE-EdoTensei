cmake_minimum_required(VERSION 3.10)
project(FILEEdo VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(-D_FILE_OFFSET_BITS=64)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -Wall")

include_directories(include)

set(SOURCES
    src/main.cpp
    src/disk_io.cpp
)

add_executable(FILEEdo ${SOURCES})
