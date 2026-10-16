CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -static

ffhost: ffhost.c demo.c ffhost.h ../ffproto.h ../../common/slgame.h
	$(CC) $(CFLAGS) -I.. -I../../common -o $@ ffhost.c demo.c $(LDFLAGS)

clean:
	rm -f ffhost
