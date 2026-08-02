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
)

add_executable(lazyplay ${SOURCES})

# Windows System Libraries
target_link_libraries(lazyplay PRIVATE
    ws2_32
    dnsapi
    d3d11
    dxva2
    mfplat
    mfuuid
    strmiids
)
