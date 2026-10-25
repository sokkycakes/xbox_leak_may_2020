CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -static

SRC := sionlinkd.c uds.c dlp.c ctrcrypto.c radio3ds.c

sionlinkd: $(SRC) uds.h dlp.h ctrcrypto.h radio3ds.h ../common/slproto.h ../common/sluds.h
	$(CC) $(CFLAGS) -I../common -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f sionlinkd
