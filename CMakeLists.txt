cmake_minimum_required(VERSION 3.20)
project(lazyplay C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /EHsc /utf-8)
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

# Vendored Fraunhofer FDK AAC decoder (decoder libs only)
file(GLOB FDK_AAC_DIRS "src/fdk-aac/*")
set(FDK_AAC_SOURCES "")
foreach(dir ${FDK_AAC_DIRS})
    if(IS_DIRECTORY ${dir})
        file(GLOB lib_srcs "${dir}/src/*.cpp")
        list(APPEND FDK_AAC_SOURCES ${lib_srcs})
    endif()
endforeach()

add_executable(lazyplay ${SOURCES} ${PLAYFAIR_SOURCES} ${FDK_AAC_SOURCES})

# fdk-aac headers live in each library's include/ and src/ directories
foreach(dir ${FDK_AAC_DIRS})
    if(IS_DIRECTORY ${dir})
        target_include_directories(lazyplay PRIVATE "${dir}/include" "${dir}/src")
    endif()
endforeach()

# Windows System Libraries
target_link_libraries(lazyplay PRIVATE
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
)
