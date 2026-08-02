CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -municode
LDFLAGS = -lws2_32 -ldnsapi -ld3d11 -ldxva2 -lmfplat -lmfuuid -lstrmiids -lgdi32 -lole32 -liphlpapi

SRCS = src/main.cpp \
       src/mdns_sd.cpp \
       src/rtsp_server.cpp \
       src/decoder_d3d11.cpp \
       src/renderer_d3d11.cpp

OBJS = $(SRCS:.cpp=.o)
TARGET = lazyplay.exe

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
