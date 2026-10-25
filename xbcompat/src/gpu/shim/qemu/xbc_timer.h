/* QEMU timers and clocks for the NV2A's PTIMER alarm (xbcompat-gpu). */
#ifndef XBC_QEMU_TIMER_H
#define XBC_QEMU_TIMER_H
#include <stdint.h>
#include <stdbool.h>

typedef enum { QEMU_CLOCK_REALTIME = 0, QEMU_CLOCK_VIRTUAL = 1, QEMU_CLOCK_HOST = 2 } QEMUClockType;
typedef void QEMUTimerCB(void *opaque);
typedef struct QEMUTimer {
    QEMUTimerCB *cb;
    void *opaque;
    int64_t expire;   /* ns, -1 when not armed */
    struct QEMUTimer *next;
} QEMUTimer;

int64_t qemu_clock_get_ns(QEMUClockType type);
static inline int64_t qemu_clock_get_us(QEMUClockType t) { return qemu_clock_get_ns(t) / 1000; }
static inline int64_t qemu_clock_get_ms(QEMUClockType t) { return qemu_clock_get_ns(t) / 1000000; }
void timer_init_ns(QEMUTimer *t, QEMUClockType type, QEMUTimerCB *cb, void *opaque);
void timer_mod(QEMUTimer *t, int64_t expire_ns);
void timer_del(QEMUTimer *t);
bool timer_pending(QEMUTimer *t);

#endif
