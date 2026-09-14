CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -march=native -pipe -pthread -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude
LDFLAGS += -pthread

BIN := build/httpd
SRCS := src/main.cpp src/http_server.cpp src/http_parser.cpp src/mime.cpp
OBJS := $(patsubst src/%.cpp,build/%.o,$(SRCS))

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

run: $(BIN)
	./$(BIN) --port 8000 --root public

clean:
	rm -rf build

.PHONY: all run clean
