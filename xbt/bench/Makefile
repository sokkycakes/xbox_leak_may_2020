# i386 Linux static freestanding benchmarks, linked low so the identity
# mapped Xbox address space can hold them (xbt --elf).
CC := clang --target=i386-linux-gnu
CFLAGS := -O2 -march=pentium3 -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -nostdlib -static
LDFLAGS := -Wl,-Ttext=0x00401000 -Wl,--build-id=none -fuse-ld=lld
all: build/b_int build/b_x87 build/b_sse build/b_null
build/%: %.c rt.c rt.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< rt.c
build/b_x87: b_fp.c rt.c rt.h
	@mkdir -p build
	$(CC) $(CFLAGS) -mno-sse -DUSE_X87 $(LDFLAGS) -o $@ b_fp.c rt.c
build/b_sse: b_fp.c rt.c rt.h
	@mkdir -p build
	$(CC) $(CFLAGS) -msse -mfpmath=sse $(LDFLAGS) -o $@ b_fp.c rt.c
clean:
	rm -rf build
