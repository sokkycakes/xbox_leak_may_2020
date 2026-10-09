# xbcompat is a 32-bit program: Xbox code runs natively in its address space.
CC      ?= gcc
CFLAGS  ?= -O2 -g
override CFLAGS += -m32 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fno-pie \
           -mstackrealign -fno-omit-frame-pointer -D_FILE_OFFSET_BITS=64
override LDFLAGS += -m32 -no-pie
PKGCFG  ?= PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config
# GL=opengl links glvnd's libOpenGL, with no GLX (KMSDRM systems, see tools/sion).
GL      ?= gl
override CFLAGS += $(shell $(PKGCFG) --cflags sdl2 $(GL))
LDLIBS  += $(shell $(PKGCFG) --libs sdl2 $(GL)) -lpthread -lm

LEAK    ?= ../xbox_leak_may_2020/xbox trunk/xbox
KERNSRC  = src/kernel/ke.c src/kernel/ob.c src/kernel/io.c src/kernel/rtl.c src/kernel/misc.c src/kernel/dvd.c \
           src/kernel/kernel.c src/kernel/av.c src/kernel/exports.gen.c
HLESRC   = src/hle/hle.c src/hle/d3d8.c src/hle/vsh.c src/hle/vsh_interp.c src/hle/psh.c src/hle/xinput.c src/hle/dsound.c
SRC      = src/main.c src/log.c src/loader.c src/memory.c src/thread.c $(KERNSRC) $(HLESRC)
BUILD   ?= build
OBJ      = $(SRC:%.c=$(BUILD)/%.o)

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
