CXX      = g++
# opencv4 is the pkg-config name on Ubuntu 20+; use opencv on older distros
OPENCV   = $(shell pkg-config --exists opencv4 && echo opencv4 || echo opencv)
CXXFLAGS = $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 $(OPENCV)) -Wall -std=c++17
LDFLAGS  = $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 $(OPENCV))

all: bin/tx bin/rx

bin/tx: src/tx.cpp
	$(CXX) src/tx.cpp -o bin/tx $(CXXFLAGS) $(LDFLAGS)

bin/rx: src/rx.cpp
	$(CXX) src/rx.cpp -o bin/rx $(CXXFLAGS) $(LDFLAGS)

clean:
	rm -f bin/tx bin/rx

.PHONY: all clean
