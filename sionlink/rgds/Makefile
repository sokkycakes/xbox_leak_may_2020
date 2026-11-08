# Sion Link client for Linux handhelds. Native: `make`. RG DS (arm64):
#   make CC=aarch64-linux-gnu-gcc PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig
# Links the system's SDL2 and SDL2_ttf at run time (BananaOS ships both).
CC      ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 SDL2_ttf)
SDL_LIBS   := $(shell $(PKG_CONFIG) --libs sdl2 SDL2_ttf)

slclient: slclient.c sllinux.c sllinux.h ../dslib/sllink.h ../common/slproto.h
	$(CC) $(CFLAGS) -I. -I../dslib -I../common $(SDL_CFLAGS) -o $@ slclient.c sllinux.c $(SDL_LIBS)

clean:
	rm -f slclient slclient-headless

# No SDL, static, --headless only: for tests (a VM with mac80211_hwsim)
slclient-headless: slclient.c sllinux.c sllinux.h ../dslib/sllink.h ../common/slproto.h
	$(CC) $(CFLAGS) -DSL_NO_SDL -I. -I../dslib -I../common -static -o $@ slclient.c sllinux.c
