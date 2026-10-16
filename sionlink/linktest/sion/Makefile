CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -static

linktest: linktest.c font8x16.h ../ltproto.h ../ltmeter.h ../../common/slgame.h
	$(CC) $(CFLAGS) -I.. -I../../common -o $@ linktest.c $(LDFLAGS)

clean:
	rm -f linktest
