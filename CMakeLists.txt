cmake_minimum_required(VERSION 3.17)

project(libavio VERSION 3.2.6)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__STDC_CONSTANT_MACROS")

list(PREPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/../cmake)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_compile_options(/EHsc /MT)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    set(BUILD_SHARED_LIBS TRUE)
endif()

find_package(FFmpeg REQUIRED)
find_package(SDL2 REQUIRED)

add_executable(test
    test.cpp
)

target_link_libraries(test PRIVATE 
    FFmpeg::FFmpeg
    SDL2::SDL2
)

target_include_directories(test SYSTEM PUBLIC
    ../include
)

add_executable(wasapi_ffmpeg_player
    wasapi_ffmpeg_player.cpp
)

target_link_libraries(wasapi_ffmpeg_player PRIVATE 
    FFmpeg::FFmpeg
    SDL2::SDL2
)

target_include_directories(wasapi_ffmpeg_player SYSTEM PUBLIC
    ../include
)
