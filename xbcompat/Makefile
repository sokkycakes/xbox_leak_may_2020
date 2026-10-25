# xbcompat is a 32-bit program: Xbox code runs natively in its address space.
# TARGET=armhf builds it for 32-bit ARM (Raspberry Pi), where the Xbox code
# runs in Unicorn instead (src/cpu.h); everything else stays the same.
# TRANSLATED=1 on i386 runs the guest in Unicorn too (for testing the
# translator, and timing it against native, on a PC).
# CPU=xlat replaces Unicorn with xbcompat's own translator (src/xlat).
RENDERER ?= builtin
ifeq ($(RENDERER),native)
ifeq ($(TARGET)$(CPU),armhfxlat)
BUILD ?= build-armhf-xlat-native
else
BUILD ?= build-native-client
endif
endif
TARGET  ?= i386
UNICORN ?= /opt/unicorn-$(TARGET)
CPU     ?= unicorn
ifeq ($(CPU),xlat)
ifeq ($(TARGET),armhf)
BUILD   ?= build-armhf-xlat
else
BUILD   ?= build-xlat
TRANSLATED = 1
endif
endif
ifeq ($(TARGET),armhf)
TRANSLATED = 1
CROSS   ?= arm-linux-gnueabihf-
CC       = $(CROSS)gcc
CFLAGS  ?= -O2 -g
# The guest owns 0x10000-0x04000000 (the XBE), so the program loads elsewhere.
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fno-pie \
           -fno-omit-frame-pointer -D_FILE_OFFSET_BITS=64 -Isrc
# No NEON: GCC tags NEON loads and stores of 8-byte-aligned types with an
# alignment hint, and guest structures are often only 4-aligned (MSVC's stack),
# which faults (SIGBUS) under a 64-bit kernel.  VFP-only code has no hints.
override CFLAGS += -mfpu=vfpv4
# ARMv7 with the divide instructions every Pi from the 2 on has (plain
# armv7-a calls a library routine for each division), scheduled for the
# Pi 3's Cortex-A53.
override CFLAGS += -march=armv7ve -mtune=cortex-a53
# char is signed on x86 (and in the Xbox code xbcompat mirrors), unsigned on ARM.
override CFLAGS += -fsigned-char
override LDFLAGS += -no-pie -Wl,-Ttext-segment=0x60000000
PKGCFG  ?= PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig pkg-config
BUILD   ?= build-armhf
else
CC      ?= gcc
CFLAGS  ?= -O2 -g
override CFLAGS += -m32 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fno-pie \
           -mstackrealign -fno-omit-frame-pointer -D_FILE_OFFSET_BITS=64
override LDFLAGS += -m32 -no-pie
PKGCFG  ?= PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config
ifeq ($(TRANSLATED),1)
override CFLAGS += -DXBC_FORCE_TRANSLATED -Isrc
BUILD   ?= build-translated
endif
BUILD   ?= build
endif
ifeq ($(TRANSLATED),1)
ifneq ($(CPU),xlat)
override CFLAGS += -I$(UNICORN)/include
LDLIBS  += $(UNICORN)/lib/libunicorn.a
endif
endif
# GL=opengl links glvnd's libOpenGL, with no GLX (KMSDRM systems, see tools/sion).
GL      ?= gl
override CFLAGS += $(shell $(PKGCFG) --cflags sdl2 $(GL))
LDLIBS  += $(shell $(PKGCFG) --libs sdl2 $(GL)) -lpthread -lm

LEAK    ?= ../xbox_leak_may_2020/xbox trunk/xbox
KERNSRC  = src/kernel/ke.c src/kernel/ob.c src/kernel/io.c src/kernel/rtl.c src/kernel/misc.c src/kernel/xcrypt.c src/kernel/dvd.c \
           src/kernel/kernel.c src/kernel/av.c src/kernel/reset.c src/kernel/irp.c src/kernel/nic.c src/kernel/net.c \
           src/kernel/exports.gen.c src/kernel/wincart.c
HLESRC   = src/hle/hle.c src/hle/d3d8.c src/hle/vsh.c src/hle/vsh_interp.c src/hle/psh.c src/hle/xinput.c src/hle/dsound.c
ifeq ($(RENDERER),native)
HLESRC := $(filter-out src/hle/d3d8.c src/hle/vsh.c src/hle/vsh_interp.c src/hle/psh.c,$(HLESRC)) src/renderer/client.c
ifeq ($(TARGET)$(CPU),armhfxlat)
# The native renderer's objects linked in, with their hidden symbols made
# local (they keep their own copies of the services backend.c forwards), so
# XBCOMPAT_RENDER_WORKER draws on its own thread here as under Box86.
override CFLAGS += -DXBC_RENDERER_STATIC
else
ifneq ($(TARGET),i386)
$(error RENDERER=native is the i386/Box86 client or the armhf xlat build; build the ARM library with tools/native-renderer.mk)
endif
ifeq ($(TRANSLATED),1)
$(error RENDERER=native cannot be combined with Unicorn TRANSLATED=1)
endif
override CFLAGS += -Isrc
LDLIBS += -ldl
endif
endif
SRC      = src/main.c src/probe.c src/log.c src/prof.c src/loader.c src/xbeimage.c src/memory.c src/thread.c $(KERNSRC) $(HLESRC)
OBJ      = $(SRC:%.c=$(BUILD)/%.o)
# The Guide overlay (src/guide, C++98 with no C++ library) on the frolickdash
# XUI runtime; the native renderer builds leave it out (d3d8.c's hook is weak).
XUIDIR   = tools/frolickdash/xui
ifneq ($(RENDERER),native)
GUIDESRC = src/guide/guide.cpp $(XUIDIR)/xui_parse.cpp $(XUIDIR)/xui_scene.cpp $(XUIDIR)/xui_draw.cpp $(XUIDIR)/xui_classes.cpp
OBJ     += $(GUIDESRC:%.cpp=$(BUILD)/%.o)
endif
ifeq ($(TARGET),armhf)
CXX      = $(CROSS)g++
CXXARCH  = -fsigned-char -mfpu=vfpv4
else
CXX      = g++
CXXARCH  = -m32 -mstackrealign
endif
CXXFLAGS = $(CXXARCH) -O2 -g -std=c++98 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-pie -fno-omit-frame-pointer \
           -D_snprintf=snprintf -Wno-misleading-indentation -I$(XUIDIR) -Isrc \
           $(shell $(PKGCFG) --cflags sdl2 $(GL))
$(BUILD)/%.o: %.cpp $(XUIDIR)/xui.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
ifeq ($(RENDERER),native)
ifeq ($(TARGET)$(CPU),armhfxlat)
RBUILD = $(BUILD)/renderer
OBJ += $(BUILD)/renderer_stubs.o $(BUILD)/renderer.o
$(BUILD)/src/renderer/client.o: src/renderer/bridge.h src/cpu.h
$(BUILD)/renderer.o: src/hle/*.c src/hle/*.h src/renderer/*.c src/renderer/*.h src/xbcompat.h src/xbox.h src/cpu.h \
                     tools/native-renderer.mk tools/gen_adapters.py
	$(MAKE) -f tools/native-renderer.mk TARGET=armhf BUILD=$(RBUILD) GL=$(GL) PKGCFG="$(PKGCFG)" objects
	$(CROSS)ld -r -o $@ $(RBUILD)/d3d8.o $(RBUILD)/vsh.o $(RBUILD)/vsh_interp.o $(RBUILD)/psh.o \
	    $(RBUILD)/backend.o $(RBUILD)/dsmix.o
	$(CROSS)objcopy --localize-hidden $@
$(BUILD)/renderer_stubs.c: src/hle/d3d8.c tools/gen_renderer_stubs.py $(BUILD)/renderer.o
	python3 tools/gen_renderer_stubs.py --translated $< $(RBUILD)/d3d8.xa.inc > $@
$(BUILD)/renderer_stubs.o: $(BUILD)/renderer_stubs.c src/cpu.h src/hle/hle.h
	$(CC) $(CFLAGS) -c $< -o $@
else
OBJ += $(BUILD)/renderer_stubs.o $(BUILD)/renderer_entry.o
$(BUILD)/src/renderer/client.o: src/renderer/bridge.h src/cpu.h
$(BUILD)/renderer_stubs.c: src/hle/d3d8.c tools/gen_renderer_stubs.py
	@mkdir -p $(BUILD)
	python3 tools/gen_renderer_stubs.py $< > $@
$(BUILD)/renderer_stubs.o: $(BUILD)/renderer_stubs.c src/renderer/bridge.h
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/renderer_entry.o: src/renderer/entry.S
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
endif
endif
ifeq ($(TRANSLATED),1)
# Each source file is compiled together with its generated guest-call
# adapters (tools/gen_adapters.py), from a wrapper that includes both.
ifeq ($(CPU),xlat)
XLATSRC  = src/xlat/decode.c src/xlat/interp.c src/xlat/fpu.c src/xlat/runtime.c
ifeq ($(TARGET),armhf)
# ... and on ARM the code generator, unless XLAT_INTERP=1 (the interpreter only)
ifneq ($(XLAT_INTERP),1)
XLATSRC += src/xlat/jit_arm.c
XLATFLAGS = -DXLAT_JIT
endif
endif
OBJ     += $(XLATSRC:%.c=$(BUILD)/%.o)
else
OBJ     += $(BUILD)/src/cpu_unicorn.o
endif
OBJ     += $(BUILD)/src/xa_format.o
$(BUILD)/src/%.o: src/%.c tools/gen_adapters.py src/xbcompat.h src/xbox.h src/cpu.h src/hle/hle.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DXBC_GEN -E -o $(BUILD)/src/$*.i $<
	python3 tools/gen_adapters.py $(BUILD)/src/$*.i $< > $(BUILD)/src/$*.xa.inc
	printf '#include "%s"\n#include "%s"\n' $(abspath $<) $(abspath $(BUILD)/src/$*.xa.inc) > $(BUILD)/src/$*.wrap.c
	$(CC) $(CFLAGS) -c -o $@ $(BUILD)/src/$*.wrap.c
$(BUILD)/src/cpu_unicorn.o: src/cpu_unicorn.c src/cpu.h src/xbcompat.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/src/xa_format.o: src/xa_format.c src/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
# On i386, SSE arithmetic: x87 excess precision would round twice.
ifeq ($(TARGET),i386)
XLATFLAGS = -msse2 -mfpmath=sse
endif
$(BUILD)/src/xlat/%.o: src/xlat/%.c src/xlat/xlat.h src/xlat/arm.h src/cpu.h src/xbcompat.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(XLATFLAGS) -c -o $@ $<
endif

.DEFAULT_GOAL := all
all: $(BUILD)/xbcompat $(BUILD)/glslcheck

$(BUILD)/xbcompat: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c src/xbcompat.h src/xbox.h src/hle/hle.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/src/kernel/ob.o $(BUILD)/src/kernel/irp.o: src/kernel/driver.h
$(BUILD)/src/hle/d3d8.o: src/hle/ff_vsh.h src/hle/dxt_decode.h src/hle/nv2a_shaders.h src/hle/async_draws.h

$(BUILD)/glslcheck: tools/glslcheck.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# Renders a Guide scene to a .ppm with xbcompat's own XUI code (tools/guide).
$(BUILD)/guideview: tools/guide/guideview.cpp src/guide/guide.cpp $(filter-out $(BUILD)/src/guide/guide.o,$(GUIDESRC:%.cpp=$(BUILD)/%.o)) $(BUILD)/src/xbeimage.o
	$(CXX) $(CXXFLAGS) -o $@ $< $(filter %.o,$^) $(LDLIBS)

src/kernel/exports.gen.c: tools/gen_exports.py
	python3 tools/gen_exports.py "$(LEAK)/private/ntos/init/ntoskrnl.src" $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
