cmake_minimum_required(VERSION 3.20)
project(lazyplay C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /EHsc /utf-8)
endif()

# FFmpeg: downloaded/built on demand for AAC-ELD decoding.
# It is built with only the native AAC decoder, which is LGPL-compatible.
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg" CACHE PATH "FFmpeg source/build directory")
set(FFMPEG_VER "7.1.1" CACHE STRING "FFmpeg version")
set(FFMPEG_TAR "${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg-${FFMPEG_VER}.tar.xz")
set(FFMPEG_URL "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VER}.tar.xz")

if(NOT EXISTS "${FFMPEG_DIR}/libavcodec/libavcodec.a" OR NOT EXISTS "${FFMPEG_DIR}/libavutil/libavutil.a")
    if(NOT EXISTS "${FFMPEG_TAR}")
        message(STATUS "Downloading FFmpeg ${FFMPEG_VER}...")
        file(DOWNLOAD ${FFMPEG_URL} ${FFMPEG_TAR} SHOW_PROGRESS)
    endif()
    if(NOT EXISTS "${FFMPEG_DIR}")
        message(STATUS "Extracting FFmpeg...")
        file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/thirdparty")
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xJf ${FFMPEG_TAR}
                        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/thirdparty"
                        COMMAND_ERROR_IS_FATAL ANY)
        file(RENAME "${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg-${FFMPEG_VER}" ${FFMPEG_DIR})
    endif()

    find_program(BASH bash)
    if(NOT BASH)
        message(FATAL_ERROR "bash not found. FFmpeg must be built from a POSIX shell (MSYS2).")
    endif()

    message(STATUS "Configuring FFmpeg (minimal AAC decoder)...")
    execute_process(
        COMMAND ${BASH} -c "./configure --disable-all --enable-avcodec --enable-avutil --enable-decoder=aac --enable-parser=aac --disable-doc --disable-programs --disable-network --disable-zlib --disable-bzlib --disable-lzma --disable-iconv --disable-xlib --disable-alsa --disable-sdl2 --disable-autodetect --disable-asm --disable-x86asm --target-os=mingw32 --arch=x86_64"
        WORKING_DIRECTORY ${FFMPEG_DIR}
        COMMAND_ERROR_IS_FATAL ANY
    )

    message(STATUS "Building FFmpeg...")
    execute_process(
        COMMAND ${CMAKE_MAKE_PROGRAM} -j4
        WORKING_DIRECTORY ${FFMPEG_DIR}
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/mdns_sd.cpp
    src/rtsp_server.cpp
    src/decoder_d3d11.cpp
    src/renderer_d3d11.cpp
    src/sha512.cpp
    src/aes_ctr.cpp
    src/aes_cbc.cpp
    src/fairplay.cpp
    src/bplist.cpp
    src/video_stream.cpp
    src/audio_stream.cpp
    src/audio_decoder.cpp
    src/audio_wasapi.cpp
    src/ntp_timing.cpp
)

# Vendored playfair (FairPlay SAP) — C sources
set(PLAYFAIR_SOURCES
    src/playfair/playfair.c
    src/playfair/hand_garble.c
    src/playfair/modified_md5.c
    src/playfair/omg_hax.c
    src/playfair/sap_hash.c
)

add_executable(lazyplay ${SOURCES} ${PLAYFAIR_SOURCES})

target_include_directories(lazyplay PRIVATE ${FFMPEG_DIR})

# FFmpeg static libraries
find_library(AVCODEC_LIB avcodec PATHS "${FFMPEG_DIR}/libavcodec" NO_DEFAULT_PATH)
find_library(AVUTIL_LIB avutil PATHS "${FFMPEG_DIR}/libavutil" NO_DEFAULT_PATH)
if(NOT AVCODEC_LIB OR NOT AVUTIL_LIB)
    message(FATAL_ERROR "FFmpeg libraries not found in ${FFMPEG_DIR}")
endif()

# Windows System Libraries
target_link_libraries(lazyplay PRIVATE
    ${AVCODEC_LIB}
    ${AVUTIL_LIB}
    ws2_32
    dnsapi
    d3d11
    d3dcompiler
    dxva2
    mfplat
    mfuuid
    strmiids
    gdi32
    ole32
    iphlpapi
    bcrypt
)
