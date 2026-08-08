CC = gcc
CXX = g++
CFLAGS = -std=c11 -O2 -w -MMD -MP
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -municode -MMD -MP
# Static-link the MinGW runtimes (libgcc/libstdc++/winpthread) so the exe
# runs on machines without a MinGW installation
LDFLAGS = -static-libgcc -static-libstdc++ -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic \
          -lws2_32 -ldnsapi -ld3d11 -ld3dcompiler -ldxva2 -lmfplat -lmfuuid -lstrmiids -lgdi32 -lole32 -liphlpapi

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

# Vendored fdk-aac (AAC-ELD decoder, Fraunhofer FDK) — decoder libs only
FDK_INC = $(foreach d,$(wildcard src/fdk-aac/*),-I$d/include -I$d/src)
FDK_CXXFLAGS = -std=c++17 -O2 -w $(FDK_INC)
FDK_SRCS = $(wildcard src/fdk-aac/*/src/*.cpp)
FDK_OBJS = $(FDK_SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS) $(FDK_OBJS)
	$(CXX) $(OBJS) $(FDK_OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/fdk-aac/%.o: src/fdk-aac/%.cpp
	$(CXX) $(FDK_CXXFLAGS) -c $< -o $@

# audio_decoder.cpp pulls in fdk-aac headers
src/audio_decoder.o: src/audio_decoder.cpp
	$(CXX) $(CXXFLAGS) $(FDK_INC) -c $< -o $@

# test_all.cpp too (adec test uses the fdk API directly)
test/test_all.o: test/test_all.cpp
	$(CXX) $(CXXFLAGS) $(FDK_INC) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

TEST_OBJS = test/test_all.o src/sha512.o src/aes_ctr.o src/aes_cbc.o src/bplist.o src/fairplay.o \
       src/decoder_d3d11.o src/renderer_d3d11.o src/audio_wasapi.o src/audio_decoder.o \
       src/playfair/playfair.o src/playfair/hand_garble.o src/playfair/modified_md5.o \
       src/playfair/omg_hax.o src/playfair/sap_hash.o

test/test_all.exe: $(TEST_OBJS) $(FDK_OBJS)
	$(CXX) $(TEST_OBJS) $(FDK_OBJS) -o $@ $(LDFLAGS)

test: test/test_all.exe
	./test/test_all.exe unit

DEPS = $(OBJS:.o=.d) $(TEST_OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(FDK_OBJS) $(TARGET) test/test_all.o test/test_all.exe $(DEPS)

-include $(DEPS)

.PHONY: all clean test
