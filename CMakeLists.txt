cmake_minimum_required(VERSION 3.15)

project(otl)

set(CMAKE_CXX_STANDARD 14)
set(UTILITY_TOP ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(${UTILITY_TOP}/bmutility)
include_directories(${FFMPEG_INCLUDE_DIRS})

add_library(otl stream_sei.cpp
        stream_demuxer.cpp
        stream_decode.cpp
        otl_timer.cpp
        otl_string.cpp
        )

