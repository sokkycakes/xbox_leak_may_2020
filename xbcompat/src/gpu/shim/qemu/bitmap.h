/* The bit of QEMU's bitmap API the NV2A code uses (xbcompat-gpu). */
#ifndef XBC_QEMU_BITMAP_H
#define XBC_QEMU_BITMAP_H
#include "qemu/osdep.h"
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BITS_TO_LONGS(n) DIV_ROUND_UP(n, BITS_PER_LONG)
#define BIT_WORD(nr) ((nr) / BITS_PER_LONG)
#define BIT_MASK(nr) (1UL << ((nr) % BITS_PER_LONG))
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
static inline int test_bit(long nr, const unsigned long *addr) { return 1UL & (addr[BIT_WORD(nr)] >> (nr & (BITS_PER_LONG - 1))); }
static inline void set_bit(long nr, unsigned long *addr) { addr[BIT_WORD(nr)] |= BIT_MASK(nr); }
static inline void clear_bit(long nr, unsigned long *addr) { addr[BIT_WORD(nr)] &= ~BIT_MASK(nr); }
static inline void bitmap_set(unsigned long *map, long start, long nr) { for (long i = 0; i < nr; i++) set_bit(start + i, map); }
static inline void bitmap_zero(unsigned long *map, long nbits) { memset(map, 0, BITS_TO_LONGS(nbits) * sizeof(unsigned long)); }
#endif
