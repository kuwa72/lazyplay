CC = gcc
CXX = g++
CFLAGS = -std=c11 -O2 -w -MMD -MP
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -municode -MMD -MP
# Static-link the MinGW runtimes (libgcc/libstdc++/winpthread) so the exe
# runs on machines without a MinGW installation
LDFLAGS = -static-libgcc -static-libstdc++ -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic \
          -lws2_32 -ldnsapi -ld3d11 -ld3dcompiler -ldxva2 -lmfplat -lmfuuid -lstrmiids -lgdi32 -lole32 -liphlpapi -lbcrypt

# FFmpeg: downloaded on demand and built with only the native AAC decoder.
# This provides a GPL/LGPL-compatible implementation
# (FFmpeg's native AAC decoder is LGPL).
FFMPEG_VER = 7.1.1
FFMPEG_DIR = thirdparty/ffmpeg
FFMPEG_TAR = thirdparty/FFmpeg-n$(FFMPEG_VER).tar.gz
# Use the GitHub auto-generated source archive for better CI/ISP reachability.
FFMPEG_URL = https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n$(FFMPEG_VER).tar.gz

FFMPEG_CONF = \
    --disable-all \
    --enable-avcodec \
    --enable-avutil \
    --enable-decoder=aac \
    --enable-parser=aac \
    --disable-doc \
    --disable-programs \
    --disable-network \
    --disable-zlib \
    --disable-bzlib \
    --disable-lzma \
    --disable-iconv \
    --disable-xlib \
    --disable-alsa \
    --disable-sdl2 \
    --disable-autodetect \
    --disable-asm \
    --disable-x86asm \
    --target-os=mingw32 \
    --arch=x86_64

FFMPEG_LIBS = $(FFMPEG_DIR)/libavcodec/libavcodec.a $(FFMPEG_DIR)/libavutil/libavutil.a
FFMPEG_INC = -I$(FFMPEG_DIR)
FFMPEG_STAMP = $(FFMPEG_DIR)/.stamp

CXX_SRCS = src/main.cpp \
       src/mdns_sd.cpp \
       src/rtsp_server.cpp \
       src/decoder_d3d11.cpp \
       src/renderer_d3d11.cpp \
       src/sha512.cpp \
       src/aes_ctr.cpp \
       src/aes_cbc.cpp \
       src/fairplay.cpp \
       src/bplist.cpp \
       src/video_stream.cpp \
       src/audio_stream.cpp \
       src/audio_decoder.cpp \
       src/audio_wasapi.cpp \
       src/ntp_timing.cpp

C_SRCS = src/playfair/playfair.c \
       src/playfair/hand_garble.c \
       src/playfair/modified_md5.c \
       src/playfair/omg_hax.c \
       src/playfair/sap_hash.c

OBJS = $(CXX_SRCS:.cpp=.o) $(C_SRCS:.c=.o)
TARGET = lazyplay.exe

all: $(TARGET)

$(FFMPEG_TAR):
	@mkdir -p thirdparty
	curl -L -o $@ $(FFMPEG_URL)

$(FFMPEG_STAMP): $(FFMPEG_TAR)
	@rm -rf $(FFMPEG_DIR) thirdparty/FFmpeg-n$(FFMPEG_VER)
	tar -xzf $< -C thirdparty
	mv thirdparty/FFmpeg-n$(FFMPEG_VER) $(FFMPEG_DIR)
	cd $(FFMPEG_DIR) && ./configure $(FFMPEG_CONF)
	$(MAKE) -C $(FFMPEG_DIR) -j4
	@touch $@

$(FFMPEG_LIBS): $(FFMPEG_STAMP)
	@test -f $@ || { rm -f $<; exit 1; }

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# audio_decoder.cpp pulls in libavcodec headers
src/audio_decoder.o: src/audio_decoder.cpp $(FFMPEG_STAMP)
	$(CXX) $(CXXFLAGS) $(FFMPEG_INC) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(FFMPEG_LIBS)
	$(CXX) $(OBJS) $(FFMPEG_LIBS) -o $(TARGET) $(LDFLAGS)

TEST_OBJS = test/test_all.o src/sha512.o src/aes_ctr.o src/aes_cbc.o src/bplist.o src/fairplay.o \
       src/decoder_d3d11.o src/renderer_d3d11.o src/audio_wasapi.o src/audio_decoder.o \
       src/playfair/playfair.o src/playfair/hand_garble.o src/playfair/modified_md5.o \
       src/playfair/omg_hax.o src/playfair/sap_hash.o

test/test_all.exe: $(TEST_OBJS) $(FFMPEG_LIBS)
	$(CXX) $(TEST_OBJS) $(FFMPEG_LIBS) -o $@ $(LDFLAGS)

test: test/test_all.exe
	./test/test_all.exe unit

DEPS = $(OBJS:.o=.d) $(TEST_OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(TARGET) test/test_all.o test/test_all.exe $(DEPS)
	rm -rf thirdparty/ffmpeg thirdparty/FFmpeg-*.tar.gz

-include $(DEPS)

.PHONY: all clean test
