# xbcompat is a 32-bit program: Xbox code runs natively in its address space.
# TARGET=armhf builds it for 32-bit ARM (Raspberry Pi), where the Xbox code
# runs in Unicorn instead (src/cpu.h); everything else stays the same.
TARGET  ?= i386
ifeq ($(TARGET),armhf)
CROSS   ?= arm-linux-gnueabihf-
CC       = $(CROSS)gcc
CFLAGS  ?= -O2 -g
# The guest owns 0x10000-0x04000000 (the XBE), so the program loads elsewhere.
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fno-pie \
           -fno-omit-frame-pointer -D_FILE_OFFSET_BITS=64 -Isrc
override LDFLAGS += -no-pie -Wl,-Ttext-segment=0x60000000
PKGCFG  ?= PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig pkg-config
UNICORN ?= /opt/unicorn-armhf
override CFLAGS += -I$(UNICORN)/include
LDLIBS  += $(UNICORN)/lib/libunicorn.a
BUILD   ?= build-armhf
else
CC      ?= gcc
CFLAGS  ?= -O2 -g
override CFLAGS += -m32 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fno-pie \
           -mstackrealign -fno-omit-frame-pointer -D_FILE_OFFSET_BITS=64
override LDFLAGS += -m32 -no-pie
PKGCFG  ?= PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config
BUILD   ?= build
endif
# GL=opengl links glvnd's libOpenGL, with no GLX (KMSDRM systems, see tools/sion).
GL      ?= gl
override CFLAGS += $(shell $(PKGCFG) --cflags sdl2 $(GL))
LDLIBS  += $(shell $(PKGCFG) --libs sdl2 $(GL)) -lpthread -lm

LEAK    ?= ../xbox_leak_may_2020/xbox trunk/xbox
KERNSRC  = src/kernel/ke.c src/kernel/ob.c src/kernel/io.c src/kernel/rtl.c src/kernel/misc.c src/kernel/dvd.c \
           src/kernel/kernel.c src/kernel/av.c src/kernel/exports.gen.c
HLESRC   = src/hle/hle.c src/hle/d3d8.c src/hle/vsh.c src/hle/vsh_interp.c src/hle/psh.c src/hle/xinput.c src/hle/dsound.c
SRC      = src/main.c src/log.c src/loader.c src/memory.c src/thread.c $(KERNSRC) $(HLESRC)
OBJ      = $(SRC:%.c=$(BUILD)/%.o)
ifeq ($(TARGET),armhf)
# Each source file is compiled together with its generated guest-call
# adapters (tools/gen_adapters.py), from a wrapper that includes both.
OBJ     += $(BUILD)/src/cpu_unicorn.o
$(BUILD)/src/%.o: src/%.c tools/gen_adapters.py src/xbcompat.h src/xbox.h src/cpu.h src/hle/hle.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DXBC_GEN -E -o $(BUILD)/src/$*.i $<
	python3 tools/gen_adapters.py $(BUILD)/src/$*.i $< > $(BUILD)/src/$*.xa.inc
	printf '#include "%s"\n#include "%s"\n' $(abspath $<) $(abspath $(BUILD)/src/$*.xa.inc) > $(BUILD)/src/$*.wrap.c
	$(CC) $(CFLAGS) -c -o $@ $(BUILD)/src/$*.wrap.c
$(BUILD)/src/cpu_unicorn.o: src/cpu_unicorn.c src/cpu.h src/xbcompat.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
endif

.DEFAULT_GOAL := all
all: $(BUILD)/xbcompat $(BUILD)/glslcheck

$(BUILD)/xbcompat: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c src/xbcompat.h src/xbox.h src/hle/hle.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/glslcheck: tools/glslcheck.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

src/kernel/exports.gen.c: tools/gen_exports.py
	python3 tools/gen_exports.py "$(LEAK)/private/ntos/init/ntoskrnl.src" $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
