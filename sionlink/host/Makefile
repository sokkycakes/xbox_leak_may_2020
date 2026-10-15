CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -static

sionlinkd: sionlinkd.c ../common/slproto.h
	$(CC) $(CFLAGS) -I../common -o $@ sionlinkd.c $(LDFLAGS)

clean:
	rm -f sionlinkd
