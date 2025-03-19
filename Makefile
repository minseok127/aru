CC = gcc
CFLAGS_RELEASE = -Wall -Wextra -O2 -std=c11 -fPIC
CFLAGS_DEBUG = -Wall -Wextra -O0 -g -pg -std=c11 -fPIC
AR = ar
RANLIB = ranlib

BUILD_MODE ?= release

ifeq ($(BUILD_MODE), release)
	CFLAGS = $(CFLAGS_RELEASE)
else ifeq ($(BUILD_MODE), debug)
	CFLAGS = $(CFLAGS_DEBUG)
else
	$(error Unknown BUILD_MODE: $(BUILD_MODE). Use 'relase' or 'debug')
endif

SRCS = aru.c atomsnap.c

OBJS = $(SRCS:.c=.o)

TARGET_STATIC = libaru.a
TARGET_SHARED = libaru.so

all: $(TARGET_STATIC) $(TARGET_SHARED)

$(TARGET_STATIC): $(OBJS)
	$(AR) rcs $@ $(OBJS)
	$(RANLIB) $@

$(TARGET_SHARED): $(OBJS)
	$(CC) -shared -o $@ $(OBJS) -lpthread

$.o: $.c
	$(CC) $(CFLAGS) -pthread -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET_STATIC) $(TARGET_SHARED)

.PHONY: all clean
