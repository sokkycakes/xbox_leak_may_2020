# Standalone shared renderer. No Unicorn dependency.
# Run from xbcompat/: make -f tools/native-renderer.mk TARGET=armhf
TARGET ?= armhf
GL ?= gl
BUILD ?= build-renderer-$(TARGET)
ifeq ($(TARGET),armhf)
CC = arm-linux-gnueabihf-gcc
ARCH_FLAGS = -mfpu=vfpv4 -fsigned-char
TRIPLE = arm-linux-gnueabihf
else ifeq ($(TARGET),i386)
CC = i686-linux-gnu-gcc
ARCH_FLAGS = -m32 -mstackrealign
TRIPLE = i386-linux-gnu
else
$(error TARGET must be armhf or i386)
endif
PKGCFG ?= PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR=/usr/lib/$(TRIPLE)/pkgconfig pkg-config
CFLAGS ?= -O2 -g
override CFLAGS += $(ARCH_FLAGS) -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -Wno-format-truncation -fPIC -fvisibility=hidden -fno-omit-frame-pointer \
    -D_FILE_OFFSET_BITS=64 -DXBC_FORCE_TRANSLATED -Isrc \
    $(shell $(PKGCFG) --cflags sdl2 $(GL))
LIBS = $(shell $(PKGCFG) --libs sdl2 $(GL)) -lpthread -lm
OBJ = $(BUILD)/d3d8.o $(BUILD)/vsh.o $(BUILD)/vsh_interp.o $(BUILD)/psh.o $(BUILD)/backend.o
HEADERS = src/xbcompat.h src/xbox.h src/cpu.h src/hle/hle.h src/hle/glcache.h \
          src/hle/nv2a_shaders.h src/renderer/bridge.h
.DEFAULT_GOAL := all
all: $(BUILD)/libxbcompat_renderer.so.1
$(BUILD)/d3d8.o: src/hle/d3d8.c src/hle/ff_vsh.h src/hle/dxt_decode.h tools/gen_adapters.py $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DXBC_GEN -E $< -o $(BUILD)/d3d8.i
	python3 tools/gen_adapters.py $(BUILD)/d3d8.i $< > $(BUILD)/d3d8.xa.inc
	printf '#include "%s"\n#include "%s"\n' $(abspath $<) $(abspath $(BUILD)/d3d8.xa.inc) > $(BUILD)/d3d8.wrap.c
	$(CC) $(CFLAGS) -c $(BUILD)/d3d8.wrap.c -o $@
$(BUILD)/%.o: src/hle/%.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/backend.o: src/renderer/backend.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/libxbcompat_renderer.so.1: $(OBJ)
	$(CC) $(ARCH_FLAGS) -shared -Wl,-z,defs,-Bsymbolic,-soname,libxbcompat_renderer.so.1 -o $@ $^ $(LIBS)
.PHONY: all

ifeq ($(TARGET),i386)
$(BUILD)/renderer-abi: tests/renderer_abi.c src/renderer/entry.S
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -fno-pie -no-pie -Wall $^ -o $@
$(BUILD)/renderer-stubs.c: src/hle/d3d8.c tools/gen_renderer_stubs.py
	@mkdir -p $(BUILD)
	python3 tools/gen_renderer_stubs.py $< > $@
$(BUILD)/renderer-smoke: tests/renderer_smoke.c src/renderer/client.c src/renderer/entry.S $(BUILD)/renderer-stubs.c src/renderer/bridge.h
	$(CC) -O2 -g -fno-pie -no-pie -mstackrealign -std=gnu11 -Wall -Wextra -Isrc \
	    $(filter %.c %.S,$^) -ldl -lpthread -lGL -o $@
check: all $(BUILD)/renderer-abi $(BUILD)/renderer-smoke
	$(BUILD)/renderer-abi
	XBCOMPAT_RENDERER_LIBRARY=$(abspath $(BUILD))/libxbcompat_renderer.so.1 $(BUILD)/renderer-smoke
check-render: all $(BUILD)/renderer-smoke
	XBCOMPAT_MSAA=1 XBCOMPAT_RENDERER_LIBRARY=$(abspath $(BUILD))/libxbcompat_renderer.so.1 xvfb-run -a $(BUILD)/renderer-smoke --render
.PHONY: check check-render
endif
