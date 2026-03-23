cmake_minimum_required(VERSION 4.3)
project(unidos)

set(CMAKE_CXX_STANDARD 11)

set(CMAKE_C_STANDARD 11)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/defs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dospath.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fcb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fdtable.c
    ${CMAKE_CURRENT_SOURCE_DIR}/global.c
    ${CMAKE_CURRENT_SOURCE_DIR}/intvec.c
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mount.c
    ${CMAKE_CURRENT_SOURCE_DIR}/psp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ints/int10.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ints/int15.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ints/int20.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ints/int21.c
    ${CMAKE_CURRENT_SOURCE_DIR}/util/uc.c
)

find_package(unicorn REQUIRED)

include_directories(.)
include_directories(ints)
include_directories(util)

include_directories(${LIBUNICORN_INCLUDE_DIR})
link_libraries(${LIBUNICORN_LIBRARY} pthread)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(unidos ${SOURCE_FILES})