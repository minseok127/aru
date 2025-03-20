CXX := g++
CXXFLAGS := -std=c++20 -O2 -Wall -pthread

ARU_TARGET := orderbook_aru
ARU_SRC := orderbook_aru.cpp

SPINLOCK_TARGET := orderbook_spinlock
SPINLOCK_SRC := orderbook_spinlock.cpp

LDFLAGS += -L../..
LDLIBS += -laru

all: $(ARU_TARGET) $(SPINLOCK_TARGET)

$(ARU_TARGET): $(ARU_SRC)
	$(CXX) $(CXXFLAGS) -o $(ARU_TARGET) $(ARU_SRC) $(LDFLAGS) -static $(LDLIBS)

$(SPINLOCK_TARGET): $(SPINLOCK_SRC)
	$(CXX) $(CXXFLAGS) -o $(SPINLOCK_TARGET) $(SPINLOCK_SRC)

clean:
	rm -f $(ARU_TARGET) $(SPINLOCK_TARGET)
