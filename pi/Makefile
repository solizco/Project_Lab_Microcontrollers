# Plain Makefile. Build ON the Pi (an x86 laptop binary will not run there):
#   sudo apt install build-essential
#   make            # builds ./brain and ./bench
#   make test       # runs all tests
#   ./bench         # FIRST THING TO RUN on the Pi: measures search speed
#   ./brain --sim   # play at the keyboard with a fake Motion Controller
#   ./brain --serial /dev/serial0
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread -Ithird_party/libdeflate
CFLAGS   ?= -O2 -Ithird_party/libdeflate
LDFLAGS  ?= -pthread

SRC_LIB  := src/ai.cpp src/game.cpp src/settings.cpp src/link.cpp src/brain.cpp
OBJ_LIB  := $(SRC_LIB:src/%.cpp=build/%.o)
SRC_UI   := src/gfx.cpp src/faces.cpp src/stats.cpp src/app.cpp
OBJ_UI   := $(SRC_UI:src/%.cpp=build/%.o)
DEFLATE  := third_party/libdeflate/lib/deflate_decompress.c third_party/libdeflate/lib/zlib_decompress.c \
            third_party/libdeflate/lib/adler32.c third_party/libdeflate/lib/utils.c \
            third_party/libdeflate/lib/arm/cpu_features.c third_party/libdeflate/lib/x86/cpu_features.c
OBJ_DEF  := $(DEFLATE:third_party/libdeflate/lib/%.c=build/deflate/%.o)

all: brain bench

build/%.o: src/%.cpp $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/deflate/%.o: third_party/libdeflate/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

brain: $(OBJ_LIB) $(OBJ_UI) $(OBJ_DEF) build/main.o
	$(CXX) $^ -o $@ $(LDFLAGS)

bench: build/ai.o build/bench.o
	$(CXX) $^ -o $@ $(LDFLAGS)

build/test_ai: tests/test_ai.cpp build/ai.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
build/test_link: tests/test_link.cpp build/link.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
build/test_brain: tests/test_brain.cpp $(OBJ_LIB)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

build/test_ui: tests/test_ui.cpp $(OBJ_LIB) $(OBJ_UI) $(OBJ_DEF)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

test: build/test_ai build/test_link build/test_brain build/test_ui
	build/test_ai && build/test_link && build/test_brain && build/test_ui

clean:
	rm -rf build brain bench

.PHONY: all test clean
