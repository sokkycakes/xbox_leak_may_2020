# Standalone NV2A translator and fixed-function regression checks.
# Run from xbcompat/: make -f tools/nv2a-check.mk check
# For the native i386 toolchain, set CC=i686-linux-gnu-gcc and PKGCFG to
# 'env PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig pkg-config'.
# check starts one Xvfb for the suite; check-display uses an existing DISPLAY.
# These are synthetic checks. They do not replace the ATG .xpu/.xvu corpus
# or game regressions, which require separately available title assets.

CC ?= cc
PKGCFG ?= pkg-config
BUILD ?= build-nv2a-check
CFLAGS ?= -O2 -g
XVFB_RUN ?= xvfb-run

SDLGL_CFLAGS := $(shell $(PKGCFG) --cflags sdl2 gl)
SDLGL_LIBS := $(shell $(PKGCFG) --libs sdl2 gl)
TEST_CFLAGS = $(CFLAGS) -std=gnu11 -Wall -Wextra
TEST_CPPFLAGS = $(CPPFLAGS) -Isrc/hle $(SDLGL_CFLAGS)
TEST_LIBS = $(SDLGL_LIBS) -lm $(LDLIBS)
SHADER_HEADERS = src/hle/nv2a_shaders.h
CHECK_BINS = $(BUILD)/dxt_test $(BUILD)/psh_render $(BUILD)/psh_modes $(BUILD)/vsh_exec \
             $(BUILD)/ff_lighting_test $(BUILD)/psh_test $(BUILD)/vsh_synth \
             $(BUILD)/glslcheck

.DEFAULT_GOAL := check
.PHONY: all check check-display
all: $(CHECK_BINS)

$(BUILD):
	@mkdir -p "$@"

$(BUILD)/dxt_test: tests/dxt_test.c src/hle/dxt_decode.h | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc/hle $(TEST_CFLAGS) $(LDFLAGS) $< -o $@

$(BUILD)/psh_render: tests/psh_render.c src/hle/psh.c $(SHADER_HEADERS) | $(BUILD)
	$(CC) $(TEST_CPPFLAGS) $(TEST_CFLAGS) $(LDFLAGS) $(filter %.c,$^) $(TEST_LIBS) -o $@

$(BUILD)/psh_modes: tests/psh_modes.c src/hle/psh.c $(SHADER_HEADERS) | $(BUILD)
	$(CC) $(TEST_CPPFLAGS) $(TEST_CFLAGS) $(LDFLAGS) $(filter %.c,$^) $(TEST_LIBS) -o $@

$(BUILD)/vsh_exec: tests/vsh_exec.c src/hle/vsh.c src/hle/vsh_interp.c $(SHADER_HEADERS) | $(BUILD)
	$(CC) $(TEST_CPPFLAGS) $(TEST_CFLAGS) $(LDFLAGS) $(filter %.c,$^) $(TEST_LIBS) -o $@

$(BUILD)/ff_lighting_test: tests/ff_lighting_test.c src/hle/ff_vsh.h | $(BUILD)
	$(CC) $(TEST_CPPFLAGS) $(TEST_CFLAGS) $(LDFLAGS) $(filter %.c,$^) $(TEST_LIBS) -o $@

$(BUILD)/psh_test: tests/psh_test.c src/hle/psh.c $(SHADER_HEADERS) | $(BUILD)
	$(CC) $(TEST_CPPFLAGS) $(TEST_CFLAGS) $(LDFLAGS) $(filter %.c,$^) $(TEST_LIBS) -o $@

$(BUILD)/vsh_synth: tests/vsh_synth.c src/hle/vsh.c $(SHADER_HEADERS) | $(BUILD)
	$(CC) $(TEST_CPPFLAGS) $(TEST_CFLAGS) $(LDFLAGS) $(filter %.c,$^) $(TEST_LIBS) -o $@

$(BUILD)/glslcheck: tools/glslcheck.c | $(BUILD)
	$(CC) $(TEST_CPPFLAGS) $(TEST_CFLAGS) $(LDFLAGS) $< $(TEST_LIBS) -o $@

check: all
	$(XVFB_RUN) -a $(MAKE) --no-print-directory -f tools/nv2a-check.mk check-display

check-display: all
	$(BUILD)/dxt_test
	$(BUILD)/psh_render
	$(BUILD)/psh_modes
	$(BUILD)/vsh_exec --selftest
	$(BUILD)/ff_lighting_test
	$(BUILD)/psh_test --synthetic $(BUILD)/glslcheck $(BUILD)/psh-synthetic
	$(BUILD)/vsh_synth -o $(BUILD)/vsh-synthetic -g $(BUILD)/glslcheck
