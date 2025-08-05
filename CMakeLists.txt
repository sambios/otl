cmake_minimum_required(VERSION 3.15)

project(otl)

set(CMAKE_CXX_STANDARD 14)

option(USE_HW_CODEC "use hardware accelerator" OFF)
option(USE_TOPS_FFMPEG "use Tops ffmpeg" OFF)

set(UTILITY_TOP ${CMAKE_CURRENT_SOURCE_DIR})

if (NOT USE_TOPS_FFMPEG)
    # Find FFmpeg using pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
            libavcodec
            libavformat
            libavfilter
            libavutil
            libswscale
            libavdevice
    )
   
    set(FFMPEG_LINK_LIBS PkgConfig::FFMPEG)

else()
    set(FFMPEG_INCLUDE_DIRS /home/jovyan/buildroot/include)
    set(FFMPEG_LINK_DIRS /opt/tops/lib)
    set(FFMPEG_LINK_LIBS avcodec avformat avfilter avutil swscale avdevice)
endif()


include_directories(${UTILITY_TOP})
include_directories(${FFMPEG_INCLUDE_DIRS})
link_directories(${FFMPEG_LINK_DIRS})

if (USE_HW_CODEC)
    set(DECODE_SRC stream_decode_hw.cpp)
else()
    set(DECODE_SRC stream_decode.cpp)
endif()

add_library(otl stream_sei.cpp
        stream_demuxer.cpp
        otl_timer.cpp
        otl_string.cpp
        optimized_timer.cpp
        otl_log.cpp
        ${DECODE_SRC}
        )

add_executable(timer_performance_test timer_performance_test.cpp)
target_link_libraries(timer_performance_test otl
	${FFMPEG_LINK_LIBS}
        pthread
)

add_executable(timestamp_test timestamp_test.cpp)
target_link_libraries(timestamp_test otl
	${FFMPEG_LINK_LIBS}
	pthread)

add_executable(log_test test_log.cpp)
target_link_libraries(log_test otl
        ${FFMPEG_LINK_LIBS}
        pthread)

add_executable(log_telnet_test test_telnet.cpp)
target_link_libraries(log_telnet_test otl
        ${FFMPEG_LINK_LIBS}
        pthread)

