CXX      = g++
CXXFLAGS = $(shell pkg-config --cflags gstreamer-1.0) -Wall -std=c++17
LDFLAGS  = $(shell pkg-config --libs gstreamer-1.0)

all: bin/tx bin/rx

bin/tx: src/tx.cpp
	$(CXX) src/tx.cpp -o bin/tx $(CXXFLAGS) $(LDFLAGS)

bin/rx: src/rx.cpp
	$(CXX) src/rx.cpp -o bin/rx $(CXXFLAGS) $(LDFLAGS)

clean:
	rm -f bin/tx bin/rx

.PHONY: all clean
