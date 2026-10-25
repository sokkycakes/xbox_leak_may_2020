/*
 * The Xbox's Ethernet controller (the nForce MCP MAC at 0xFEF00000).
 *
 * Titles link the network stack (XNet) and its NIC driver statically, so the
 * driver programs the controller's registers itself (the leak's
 * private/ntos/net/nicx.cpp, registers in private/ntos/inc/phy.h).  Emulating
 * the controller lets every title's own TCP/IP and Xbox Live code run
 * untouched; frames go to a host backend (net.c).
 *
 * Register accesses trap: the page is mapped PROT_NONE, and a fault in it
 * fills the page with the current register values, makes it writable and
 * single-steps the instruction with the trap flag.  Afterwards a store (the
 * instruction is decoded just far enough to know) is applied as a register
 * write, with the hardware's write-1-to-clear semantics where they apply, and
 * the page is locked again.
 *
 * Descriptor rings live in contiguous memory (physical = virtual & 0x7FFFFFFF),
 * which the driver owns through the OWN bits; the controller side here polls
 * the transmit ring when the driver kicks it and on a 1 ms tick, and fills
 * receive descriptors as frames arrive.  The interrupt is delivered on its own
 * thread by calling the ISR the driver connected (KeInitializeInterrupt).
 *
 * Native (i386) builds only: translated builds keep the old inert window.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>

#include "../xbcompat.h"
#include "../cpu.h"
#include "net.h"

#define NIC_BASE     0xFEF00000u
#define NIC_IRQ      4

/* Register offsets (phy.h) */
enum {
    R_INTR = 0x000, R_INTR_MK = 0x004, R_SWTR_CNTL = 0x008, R_SWTR_ITC = 0x00c,
    R_TX_CNTL = 0x080, R_TX_EN = 0x084, R_TX_STA = 0x088, R_RX_CNTL_0 = 0x08c,
    R_RX_CNTL_1 = 0x090, R_RX_EN = 0x094, R_RX_STA = 0x098,
    R_UNI0 = 0x0a8, R_UNI1 = 0x0ac, R_MULT0 = 0x0b0, R_MULT1 = 0x0b4,
    R_MULT_MK0 = 0x0b8, R_MULT_MK1 = 0x0bc,
    R_TX_DADR = 0x100, R_RX_DADR = 0x104, R_DLEN = 0x108, R_TX_POLL = 0x10c,
    R_RX_POLL = 0x110, R_TX_CUR_DADR = 0x11c, R_RX_CUR_DADR = 0x120,
    R_MODE = 0x144, R_MINTR = 0x180, R_MINTR_MK = 0x184, R_MII_CS = 0x188,
    R_MII_TM = 0x18c, R_MDIO_ADR = 0x190, R_MDIO_DATA = 0x194,
};
#define INTR_MINT   (1u << 6)
#define INTR_TCINT  (1u << 4)
#define INTR_MISS   (1u << 2)
#define INTR_RCINT  (1u << 1)
#define MODE_DMA_IDLE  (1u << 3)
#define MODE_RXDM      (1u << 1)
#define MODE_TXDM      (1u << 0)
#define MIICS_LS       (1u << 2)
#define RXDESC_OWN  (1u << 31)
#define RXDESC_PAM  (1u << 22)
#define RXDESC_MAM  (1u << 21)
#define RXDESC_BAM  (1u << 20)
#define RXDESC_REND (1u << 16)
#define TXDESC_OWN  (1u << 31)
#define TXDESC_ERR  (1u << 30)
#define TXDESC_TEND (1u << 16)

typedef struct { volatile uint32_t addr, flags_count; } nic_desc;

bool g_nic_enabled;

static pthread_mutex_t nic_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;   /* net_send may answer at once */
static pthread_cond_t irq_cond XBC_COND_ALIGN = PTHREAD_COND_INITIALIZER;
static uint32_t regs[0x400 / 4];
static unsigned tx_index, rx_index;
static uint8_t mac[6];

/* Frames waiting for a free receive descriptor. */
#define RXQ_MAX 128
static struct { uint16_t len; uint8_t data[NET_MAX_FRAME]; } rxq[RXQ_MAX];
static unsigned rxq_head, rxq_tail;

/* The connected interrupt. */
typedef BOOLEAN (NTAPI *isr_fn)(PVOID Interrupt, PVOID Context);
static PVOID irq_object;
static isr_fn irq_routine;
static PVOID irq_context;
static bool irq_connected;

static inline uint32_t reg(unsigned off) { return regs[off / 4]; }
static inline void set_reg(unsigned off, uint32_t v) { regs[off / 4] = v; }

/* MmGetPhysicalAddress clears bit 31: contiguous memory (0x80000000 up) gives
   addresses below CONTIG_SIZE, anything else maps to itself.  Packet buffers
   come from contiguous memory or the pool, never from the image. */
static inline void *phys_to_virt(uint32_t pa)
{
    return (void *)(uintptr_t)(pa < CONTIG_SIZE ? (pa | CONTIG_BASE) : pa);
}

static unsigned tx_ring_len(void) { return (reg(R_DLEN) & 0x3FF) + 1; }
static unsigned rx_ring_len(void) { return ((reg(R_DLEN) >> 16) & 0x3FF) + 1; }

static void raise_intr(uint32_t bits)
{
    set_reg(R_INTR, reg(R_INTR) | bits);
    pthread_cond_broadcast(&irq_cond);
}

/* ---- transmit and receive (nic_lock held) ----------------------------- */

static void tx_process(void)
{
    if (!(reg(R_TX_EN) & 1) || !reg(R_TX_DADR)) return;
    static uint8_t frame[NET_MAX_FRAME];
    static unsigned frame_len;
    unsigned n = tx_ring_len();
    bool done = false;
    for (unsigned guard = 0; guard < n; guard++) {
        nic_desc *d = (nic_desc *)phys_to_virt(reg(R_TX_DADR)) + tx_index;
        uint32_t fc = d->flags_count;
        if (!(fc & TXDESC_OWN)) break;
        unsigned len = (fc & 0xFFFF) + 1;
        if (frame_len + len <= sizeof frame) memcpy(frame + frame_len, phys_to_virt(d->addr), len);
        frame_len += len;
        if (fc & TXDESC_TEND) {
            if (frame_len <= sizeof frame) net_send(frame, frame_len);
            frame_len = 0;
        }
        d->flags_count = fc & ~(TXDESC_OWN | TXDESC_ERR);
        tx_index = (tx_index + 1) % n;
        set_reg(R_TX_CUR_DADR, reg(R_TX_DADR) + tx_index * sizeof(nic_desc));
        done = true;
    }
    if (done) raise_intr(INTR_TCINT);
}

static bool rx_accepts(const uint8_t *f)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    if (!memcmp(f, bcast, 6)) return true;
    uint8_t uni[6];
    uint32_t u0 = reg(R_UNI0), u1 = reg(R_UNI1);
    memcpy(uni, &u0, 4);
    memcpy(uni + 4, &u1, 2);
    if (!memcmp(f, uni, 6)) return true;
    if (f[0] & 1) {   /* multicast: match under the mask */
        uint8_t m[6], mk[6];
        uint32_t m0 = reg(R_MULT0), m1 = reg(R_MULT1), k0 = reg(R_MULT_MK0), k1 = reg(R_MULT_MK1);
        memcpy(m, &m0, 4); memcpy(m + 4, &m1, 2); memcpy(mk, &k0, 4); memcpy(mk + 4, &k1, 2);
        for (int i = 0; i < 6; i++)
            if ((f[i] & mk[i]) != (m[i] & mk[i])) return false;
        return true;
    }
    return false;
}

/* Move queued frames into free receive descriptors. */
static void rx_process(void)
{
    if (!(reg(R_RX_EN) & 1) || !reg(R_RX_DADR)) return;
    unsigned n = rx_ring_len();
    bool done = false;
    while (rxq_head != rxq_tail) {
        nic_desc *d = (nic_desc *)phys_to_virt(reg(R_RX_DADR)) + rx_index;
        uint32_t fc = d->flags_count;
        if (!(fc & RXDESC_OWN)) break;   /* ring full: the driver hasn't returned it */
        unsigned room = (fc & 0xFFFF) + 1;
        unsigned len = rxq[rxq_head % RXQ_MAX].len;
        const uint8_t *data = rxq[rxq_head % RXQ_MAX].data;
        if (len > room) len = room;
        memcpy(phys_to_virt(d->addr), data, len);
        uint32_t flags = RXDESC_REND;
        flags |= (data[0] & 1) ? (data[0] == 0xff ? RXDESC_BAM : RXDESC_MAM) : RXDESC_PAM;
        d->flags_count = flags | len;
        rx_index = (rx_index + 1) % n;
        set_reg(R_RX_CUR_DADR, reg(R_RX_DADR) + rx_index * sizeof(nic_desc));
        rxq_head++;
        done = true;
    }
    if (done) raise_intr(INTR_RCINT);
}

void nic_receive(const uint8_t *frame, unsigned len)
{
    if (len < 14 || len > NET_MAX_FRAME) return;
    pthread_mutex_lock(&nic_lock);
    if (rx_accepts(frame)) {
        if (rxq_tail - rxq_head >= RXQ_MAX) {
            raise_intr(INTR_MISS);
        } else {
            unsigned slot = rxq_tail++ % RXQ_MAX;
            unsigned n = len < 60 ? 60 : len;   /* pad runts, as the wire would */
            memset(rxq[slot].data, 0, n);
            memcpy(rxq[slot].data, frame, len);
            rxq[slot].len = n;
            rx_process();
        }
    }
    pthread_mutex_unlock(&nic_lock);
}

void nic_poll(void)
{
    pthread_mutex_lock(&nic_lock);
    tx_process();
    rx_process();
    pthread_mutex_unlock(&nic_lock);
}

/* ---- registers (nic_lock held) --------------------------------------- */

static uint32_t reg_read(unsigned off)
{
    switch (off) {
    case R_TX_STA: case R_RX_STA: return reg(off) & ~1u;   /* never busy */
    case R_MODE: return reg(off) | MODE_DMA_IDLE;
    case R_MII_CS: return reg(off) | MIICS_LS;               /* link up */
    case R_MII_TM: return reg(off) & ~(1u << 15);           /* MII never busy */
    default: return reg(off);
    }
}

static void reg_write(unsigned off, uint32_t v)
{
    switch (off) {
    case R_INTR: case R_MINTR: case R_TX_STA: case R_RX_STA:
        set_reg(off, reg(off) & ~v);   /* write 1 to clear */
        break;
    case R_MODE:
        if (v & (1u << 4)) {   /* MODE_RESET_BUFFERS */
            tx_index = rx_index = 0;
            rxq_head = rxq_tail;
        }
        set_reg(off, v);
        if (v & MODE_TXDM) tx_process();
        if (v & MODE_RXDM) rx_process();
        break;
    case R_TX_DADR: set_reg(off, v); tx_index = 0; break;
    case R_RX_DADR: set_reg(off, v); rx_index = 0; break;
    case R_INTR_MK:
        set_reg(off, v);
        pthread_cond_broadcast(&irq_cond);
        break;
    case R_TX_EN: set_reg(off, v); if (v & 1) tx_process(); break;
    case R_RX_EN: set_reg(off, v); if (v & 1) rx_process(); break;
    default: set_reg(off, v); break;
    }
}

/* ---- the access trap -------------------------------------------------- */

static __thread bool stepping;
static __thread unsigned step_off;
static __thread bool step_store;

/* Whether the instruction at ip writes its r/m memory operand.  Covers the
   forms compilers emit for volatile register access. */
static bool insn_stores(const uint8_t *ip)
{
    while (*ip == 0x66 || *ip == 0x67 || *ip == 0xF0 || *ip == 0xF2 || *ip == 0xF3 || *ip == 0x2E ||
           *ip == 0x36 || *ip == 0x3E || *ip == 0x26 || *ip == 0x64 || *ip == 0x65)
        ip++;
    uint8_t op = ip[0], r = (ip[1] >> 3) & 7;
    if (op == 0x0F) {
        uint8_t op2 = ip[1];
        return op2 == 0xB0 || op2 == 0xB1 || op2 == 0xC0 || op2 == 0xC1 || op2 == 0xAB || op2 == 0xB3 ||
               op2 == 0xBB || (op2 == 0xBA && ((ip[2] >> 3) & 7) >= 5) || (op2 >= 0x90 && op2 <= 0x9F);
    }
    if (op < 0x40 && (op & 7) <= 1) return (op & 0x38) != 0x38;   /* ALU r/m, reg (not cmp) */
    switch (op) {
    case 0x88: case 0x89: case 0xC6: case 0xC7: case 0x86: case 0x87: return true;
    case 0x80: case 0x81: case 0x83: return r != 7;
    case 0xF6: case 0xF7: return r == 2 || r == 3;
    case 0xFE: case 0xFF: return r <= 1;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xC0: case 0xC1: return true;
    case 0xA2: case 0xA3: case 0xAA: case 0xAB: case 0xA4: case 0xA5: return true;
    default: return false;
    }
}

bool nic_trap_fault(void *addr, void *ucv)
{
#ifdef XBC_NATIVE
    uint32_t a = (uint32_t)(uintptr_t)addr;
    if (!g_nic_enabled || a < NIC_BASE || a >= NIC_BASE + 0x1000) return false;
    greg_t *r = ((ucontext_t *)ucv)->uc_mcontext.gregs;
    pthread_mutex_lock(&nic_lock);
    mprotect((void *)NIC_BASE, 0x1000, PROT_READ | PROT_WRITE);
    uint32_t *page = (uint32_t *)NIC_BASE;
    for (unsigned off = 0; off < sizeof regs; off += 4) page[off / 4] = reg_read(off);
    step_off = (a - NIC_BASE) & ~3u;
    step_store = insn_stores((const uint8_t *)r[REG_EIP]);
    stepping = true;
    r[REG_EFL] |= 0x100;   /* TF: trap after this one instruction */
    /* nic_lock stays held until the step completes. */
    return true;
#else
    (void)addr; (void)ucv;
    return false;
#endif
}

bool nic_trap_step(void *ucv)
{
#ifdef XBC_NATIVE
    if (!stepping) return false;
    greg_t *r = ((ucontext_t *)ucv)->uc_mcontext.gregs;
    r[REG_EFL] &= ~0x100;
    stepping = false;
    if (step_store && step_off < sizeof regs) {
        uint32_t v = ((volatile uint32_t *)NIC_BASE)[step_off / 4];
        reg_write(step_off, v);
    }
    mprotect((void *)NIC_BASE, 0x1000, PROT_NONE);
    pthread_mutex_unlock(&nic_lock);
    return true;
#else
    (void)ucv;
    return false;
#endif
}

/* ---- interrupt -------------------------------------------------------- */

void nic_interrupt_init(PVOID Interrupt, PVOID ServiceRoutine, PVOID ServiceContext, ULONG Vector)
{
    if (Vector != 0x30 + NIC_IRQ) return;
    pthread_mutex_lock(&nic_lock);
    irq_object = Interrupt;
    irq_routine = (isr_fn)ServiceRoutine;
    irq_context = ServiceContext;
    pthread_mutex_unlock(&nic_lock);
}

bool nic_interrupt_connect(PVOID Interrupt, bool connect)
{
    if (!g_nic_enabled || Interrupt != irq_object || !irq_object) return false;
    pthread_mutex_lock(&nic_lock);
    irq_connected = connect;
    pthread_cond_broadcast(&irq_cond);
    pthread_mutex_unlock(&nic_lock);
    if (connect) {
        xlog("nic: driver connected its interrupt (MAC %02x:%02x:%02x:%02x:%02x:%02x)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ke_dispatch_exclusive();
    }
    return true;
}

static void *irq_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "nic-irq");
    thread_adopt_host("nic-irq");
    pthread_mutex_lock(&nic_lock);
    for (;;) {
        while (!irq_connected || !(reg(R_INTR) & reg(R_INTR_MK)))
            pthread_cond_wait(&irq_cond, &nic_lock);
        PVOID obj = irq_object, ctx = irq_context;
        isr_fn isr = irq_routine;
        pthread_mutex_unlock(&nic_lock);
        ke_dispatch_enter(26 - NIC_IRQ);
        isr(obj, ctx);
        ke_dispatch_leave();
        pthread_mutex_lock(&nic_lock);
        /* An ISR that leaves the line asserted (it didn't mask) would spin. */
        if (reg(R_INTR) & reg(R_INTR_MK)) {
            pthread_mutex_unlock(&nic_lock);
            struct timespec ts = { 0, 1000000 };
            nanosleep(&ts, NULL);
            pthread_mutex_lock(&nic_lock);
        }
    }
    return NULL;
}

static void *tick_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "nic-tick");
    for (;;) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
        nic_poll();
    }
    return NULL;
}

/* ---- setup ------------------------------------------------------------ */

void nic_init(const char *spec, const uint8_t hwaddr[6])
{
    memcpy(mac, hwaddr, 6);
#ifndef XBC_NATIVE
    if (spec && strcmp(spec, "none")) xlog("nic: networking needs the native (i386) build; ignoring --net");
    return;
#else
    if (!spec || !strcmp(spec, "none")) return;
    if (!net_init(spec, mac)) return;
    if (memcmp(mac, hwaddr, 6)) identity_set_mac(mac);
    void *p = mmap((void *)NIC_BASE, 0x1000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p != (void *)NIC_BASE) { xlog("nic: cannot map the register page: %s", strerror(errno)); return; }
    g_nic_enabled = true;
    pthread_t th;
    pthread_create(&th, NULL, irq_thread, NULL);
    pthread_create(&th, NULL, tick_thread, NULL);
    xlog("nic: emulating the Ethernet controller, backend %s", spec);
#endif
}
