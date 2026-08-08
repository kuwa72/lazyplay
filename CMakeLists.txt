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
    src/fairplay.cpp
    src/bplist.cpp
    src/video_stream.cpp
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
