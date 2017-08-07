cmake_minimum_required(VERSION 3.6)
project(async_tiled)

# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++1z")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -Werror -Wall -Wextra")

set(THIRDPARTY_DIR ${PROJECT_SOURCE_DIR}/thirdparty)

# Global include directories for all targets:
include_directories(${THIRDPARTY_DIR})

set(SOURCE_FILES
    thirdparty/stb/stb_image_write.h
    main.cpp scrap.h async_tiled.h fractals.h)

add_executable(async_tiled ${SOURCE_FILES})
