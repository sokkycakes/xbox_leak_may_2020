# Builds on any x86-64 glibc distro with libdrm, Mesa EGL/GLES2 and gbm headers;
# the binary runs on the Sion (glibc 2.44, Mesa iris).
CFLAGS ?= -O2 -Wall
CFLAGS += $(shell pkg-config --cflags libdrm)
LIBS = -lEGL -lGLESv2 -lgbm -ldrm -lm
all: sion-groovy groovy-fake-mister
sion-groovy: sion-groovy.c capture.c lz4.c lz4.h
	$(CC) $(CFLAGS) -o $@ sion-groovy.c capture.c lz4.c $(LIBS)
clean:
	rm -f sion-groovy
.PHONY: all clean groovy-fake-mister
groovy-fake-mister:
	@true
