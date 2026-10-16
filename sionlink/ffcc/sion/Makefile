CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -static

ffhost: ffhost.c ../ffproto.h ../../common/slgame.h
	$(CC) $(CFLAGS) -I.. -I../../common -o $@ ffhost.c $(LDFLAGS)

clean:
	rm -f ffhost
