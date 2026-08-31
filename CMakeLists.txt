cmake_minimum_required(VERSION 3.10)
project(FILEEdo VERSION 1.0.0 LANGUAGES CXX)

option(FILEEDO_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_definitions(-D_FILE_OFFSET_BITS=64)
if(FILEEDO_ENABLE_SANITIZERS)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/app)
else()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/app)
endif()

set(SOURCES
    src/main.cpp
    src/carver.cpp
    src/searcher.cpp
    src/formats.cpp
    src/io.cpp
    src/report.cpp
    src/disk_io.cpp
)   

add_executable(FILEEdo ${SOURCES})

target_include_directories(FILEEdo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(FILEEdo PRIVATE -Wall -Wextra -Wpedantic)
if(FILEEDO_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(FILEEdo PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_libraries(FILEEdo PRIVATE -fsanitize=address,undefined)
endif()

include(CTest)
if(BUILD_TESTING)
    find_program(PYTHON_EXECUTABLE python3 REQUIRED)
    add_executable(FILEEdoTests
        test/tests.cpp
        src/searcher.cpp
        src/formats.cpp
        src/io.cpp
        src/report.cpp
        src/disk_io.cpp
    )
    target_include_directories(FILEEdoTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(FILEEdoTests PRIVATE -Wall -Wextra -Wpedantic)
    if(FILEEDO_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(FILEEdoTests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_libraries(FILEEdoTests PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME unit COMMAND FILEEdoTests)
    add_test(NAME integration
        COMMAND ${CMAKE_COMMAND}
            -DPROGRAM=$<TARGET_FILE:FILEEdo>
            -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/integration
            -DPYTHON=${PYTHON_EXECUTABLE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/integration.cmake)
endif()
