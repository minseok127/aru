CXX := g++
CXXFLAGS := -std=c++20 -O2 -g -Wall -pthread

ARU_TARGET := orderbook_aru
ARU_SRC := orderbook_aru.cpp

LDFLAGS += -L../..
LDLIBS += -laru

all: $(ARU_TARGET)

$(ARU_TARGET): $(ARU_SRC)
	$(CXX) $(CXXFLAGS) -o $(ARU_TARGET) $(ARU_SRC) $(LDFLAGS) -static $(LDLIBS)

clean:
	rm -f $(ARU_TARGET)
