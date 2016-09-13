INCLUDEFLAGS = -I ../zeromq/include
LDFLAGS = -L ../zeromq/lib
LIBFLAGS = -lzmq
CXXFLAGS = -std=c++11 -Wall -Wfatal-errors

all: test

test: test.cpp
	g++ $(CXXFLAGS) $(INCLUDEFLAGS) $^ -o $@ $(LDFLAGS) $(LIBFLAGS)

