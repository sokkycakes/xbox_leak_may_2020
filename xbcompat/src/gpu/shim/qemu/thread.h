/* QEMU threads, mutexes, conditions and events on pthreads (xbcompat-gpu). */
#ifndef XBC_QEMU_THREAD_H
#define XBC_QEMU_THREAD_H
#include <pthread.h>
#include <stdbool.h>

typedef struct QemuMutex { pthread_mutex_t m; } QemuMutex;
typedef struct QemuRecMutex { pthread_mutex_t m; } QemuRecMutex;
typedef struct QemuCond { pthread_cond_t c; } QemuCond;
typedef struct QemuEvent { pthread_mutex_t m; pthread_cond_t c; bool set; } QemuEvent;
typedef struct QemuSpin { pthread_spinlock_t s; } QemuSpin;
typedef struct QemuThread { pthread_t t; } QemuThread;

#define QEMU_THREAD_JOINABLE 0
#define QEMU_THREAD_DETACHED 1

static inline void qemu_mutex_init(QemuMutex *m) { pthread_mutex_init(&m->m, NULL); }
static inline void qemu_mutex_destroy(QemuMutex *m) { pthread_mutex_destroy(&m->m); }
static inline void qemu_mutex_lock(QemuMutex *m) { pthread_mutex_lock(&m->m); }
static inline int qemu_mutex_trylock(QemuMutex *m) { return pthread_mutex_trylock(&m->m); }
static inline void qemu_mutex_unlock(QemuMutex *m) { pthread_mutex_unlock(&m->m); }
static inline void qemu_cond_init(QemuCond *c) { pthread_cond_init(&c->c, NULL); }
static inline void qemu_cond_destroy(QemuCond *c) { pthread_cond_destroy(&c->c); }
static inline void qemu_cond_signal(QemuCond *c) { pthread_cond_signal(&c->c); }
static inline void qemu_cond_broadcast(QemuCond *c) { pthread_cond_broadcast(&c->c); }
static inline void qemu_cond_wait(QemuCond *c, QemuMutex *m) { pthread_cond_wait(&c->c, &m->m); }
bool qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms);

static inline void qemu_event_init(QemuEvent *e, bool init)
{
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->set = init;
}
static inline void qemu_event_destroy(QemuEvent *e) { (void)e; }
static inline void qemu_event_set(QemuEvent *e)
{
    pthread_mutex_lock(&e->m);
    e->set = true;
    pthread_cond_broadcast(&e->c);
    pthread_mutex_unlock(&e->m);
}
static inline void qemu_event_reset(QemuEvent *e)
{
    pthread_mutex_lock(&e->m);
    e->set = false;
    pthread_mutex_unlock(&e->m);
}
static inline void qemu_event_wait(QemuEvent *e)
{
    pthread_mutex_lock(&e->m);
    while (!e->set) pthread_cond_wait(&e->c, &e->m);
    pthread_mutex_unlock(&e->m);
}

static inline void qemu_spin_init(QemuSpin *s) { pthread_spin_init(&s->s, 0); }
static inline void qemu_spin_lock(QemuSpin *s) { pthread_spin_lock(&s->s); }
static inline void qemu_spin_unlock(QemuSpin *s) { pthread_spin_unlock(&s->s); }

void qemu_thread_create(QemuThread *t, const char *name, void *(*fn)(void *), void *arg, int mode);
static inline void *qemu_thread_join(QemuThread *t) { void *r; pthread_join(t->t, &r); return r; }

/* The big QEMU lock: there is no machine around the device here, so it only
   orders the few places that take it against each other. */
void bql_lock(void);
void bql_unlock(void);
bool bql_locked(void);

#endif
