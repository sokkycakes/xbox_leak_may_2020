# Sion Link for the RG DS (native): `make`; arm64 for the RG DS:
#   make CC=aarch64-linux-gnu-gcc PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig PKG_CONFIG_LIBDIR=...
# The DS app's screens come from mirror/ds/source/nitroui.c (and its baked art), built on shim/.
CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
DS = ../../mirror/ds/source
INC = -Ishim -I.. -I$(DS) -I../../mirror/common -I../../common -I../../dslib
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl2)

sionlink-app: linkapp.c ../sllinux.c ../sllinux.h $(DS)/nitroui.c $(DS)/nitroui.h $(DS)/nitroart.h shim/nds.h
	$(CC) $(CFLAGS) $(INC) $(SDL_CFLAGS) -o $@ linkapp.c ../sllinux.c $(DS)/nitroui.c $(SDL_LIBS) -lpthread

clean:
	rm -f sionlink-app
