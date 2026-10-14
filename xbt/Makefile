# Build the translator for an AArch64 Linux host.
# On an x86-64 workstation this cross-compiles; run with qemu-aarch64.
CC_HOST    := $(shell uname -m)
ifeq ($(CC_HOST),aarch64)
CC         ?= cc
RUN        :=
else
CC         := aarch64-linux-gnu-gcc
RUN        := qemu-aarch64 -L /usr/aarch64-linux-gnu
endif
CFLAGS     ?= -O2 -g
CFLAGS     += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing
LDFLAGS    += -lm

SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))

all: build/xbt

build/%.o: src/%.c src/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/xbt: $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

clean:
	rm -rf build

.PHONY: all clean
