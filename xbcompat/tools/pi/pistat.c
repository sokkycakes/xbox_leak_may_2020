/*
 * pistat [SECONDS] [MS]: what a Raspberry Pi's clocks really are while a
 * game runs, a line every MS milliseconds (default 1000) for SECONDS
 * (default 60).  clocks.sh reads the kernel's idea of the clock rates, which
 * is only what was asked for: the firmware lowers the ARM, core and SDRAM
 * clocks on its own as the SoC nears 85 C and doesn't tell the kernel.
 * Columns:
 *   mono   CLOCK_MONOTONIC seconds, the clock xbcompat's log stamps lines with
 *   loop   ARM MHz from timing a chain of dependent adds (the fastest of 16
 *          short runs, so being preempted doesn't count)
 *   pmu    ARM MHz from the CPU's own cycle counter over those runs (perf),
 *          "-" if the kernel has no PMU support
 *   arm core v3d sdram   the firmware's measured clocks in MHz (mailbox,
 *          /dev/vcio), "-" without it
 *   thr    the firmware's throttle flags: bit 0 under-voltage, 1 ARM
 *          clock capped, 2 throttled, 3 soft temperature limit, now;
 *          bits 16-19 the same, since the kernel last asked
 *   temp   the SoC's temperature
 * Costs about 3 ms of one core a second.
 *
 * Built static so it runs on any Pi image:
 *   arm-linux-gnueabihf-gcc -O2 -static -o pistat pistat.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define TRIALS 16
#define ITERS 2000          /* 100 adds each: ~170 us a run at 1.2 GHz */

static double now_s(clockid_t c)
{
	struct timespec ts;
	clock_gettime(c, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ITERS * 100 dependent adds: one a cycle on a Cortex-A53, the loop's own
 * subs/bne issue alongside them. */
static uint32_t chain(uint32_t n)
{
	uint32_t x = 1;
#ifdef __arm__
	__asm__ volatile(
		"1:\n"
		".rept 100\n"
		"add %0, %0, %2\n"
		".endr\n"
		"subs %1, %1, #1\n"
		"bne 1b\n"
		: "+r"(x), "+r"(n) : "r"(3) : "cc");
#else
	while (n--)
		for (int i = 0; i < 100; i++)
		{
			__asm__ volatile("" : "+r"(x));
			x += 3;
		}
#endif
	return x;
}

static int pmu_fd = -1;

static void pmu_open(void)
{
	struct perf_event_attr a;
	memset(&a, 0, sizeof a);
	a.size = sizeof a;
	a.type = PERF_TYPE_HARDWARE;
	a.config = PERF_COUNT_HW_CPU_CYCLES;
	a.exclude_kernel = 1;
	a.exclude_hv = 1;
	pmu_fd = syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
	if (pmu_fd < 0)
		printf("# pmu: none (%s)\n", strerror(errno));
	else
		printf("# pmu: cycle counter ok\n");
}

static uint64_t pmu_read(void)
{
	uint64_t v = 0;
	if (pmu_fd >= 0 && read(pmu_fd, &v, sizeof v) != sizeof v)
		v = 0;
	return v;
}

/* fastest run as MHz, by wall time and by the cycle counter */
static void measure(double *loop_mhz, double *pmu_mhz)
{
	double best = 1e9, best_pmu = 0;
	for (int t = 0; t < TRIALS; t++) {
		uint64_t c0 = pmu_read();
		double t0 = now_s(CLOCK_MONOTONIC);
		chain(ITERS);
		double dt = now_s(CLOCK_MONOTONIC) - t0;
		uint64_t c1 = pmu_read();
		if (dt < best)
			best = dt;
		if (pmu_fd >= 0 && dt > 0) {
			double m = (c1 - c0) / dt / 1e6;
			if (m > best_pmu)
				best_pmu = m;
		}
	}
	*loop_mhz = ITERS * 100.0 / best / 1e6;
	*pmu_mhz = best_pmu;
}

/* The firmware's property mailbox through the vcio driver.  The ioctl
 * number is the one the kernel's 32-bit compat path expects too. */
#define IOCTL_MBOX_PROPERTY _IOWR(100, 0, char *)
#define TAG_GET_TEMPERATURE 0x00030006
#define TAG_GET_THROTTLED 0x00030046
#define TAG_GET_CLOCK_MEASURED 0x00030047
static int mbox_fd = -1;

static int mbox(uint32_t tag, uint32_t *v, int nreq, int nval)
{
	uint32_t b[16] __attribute__((aligned(16)));
	int i = 0;
	if (mbox_fd < 0)
		return -1;
	b[i++] = 0;
	b[i++] = 0;
	b[i++] = tag;
	b[i++] = nval * 4;
	b[i++] = nreq * 4;
	for (int k = 0; k < nval; k++)
		b[i++] = k < nreq ? v[k] : 0;
	b[i++] = 0;
	b[0] = i * 4;
	if (ioctl(mbox_fd, IOCTL_MBOX_PROPERTY, b) < 0 || b[1] != 0x80000000u ||
	    !(b[4] & 0x80000000u))
		return -1;
	for (int k = 0; k < nval; k++)
		v[k] = b[5 + k];
	return 0;
}

static long clock_mhz(uint32_t id)
{
	uint32_t v[2] = { id, 0 };
	if (mbox(TAG_GET_CLOCK_MEASURED, v, 1, 2) < 0 || v[0] != id)
		return -1;
	return (v[1] + 500000) / 1000000;
}

static const char *throttled_sysfs =
	"/sys/devices/platform/soc/soc:firmware/get_throttled";

static long throttled(void)
{
	uint32_t v[1] = { 0 };    /* 0: leave the since-boot bits alone */
	if (mbox(TAG_GET_THROTTLED, v, 1, 1) == 0)
		return v[0];
	FILE *f = fopen(throttled_sysfs, "r");
	long r = -1;
	if (f) {
		unsigned long u;
		if (fscanf(f, "%lx", &u) == 1)
			r = (long)u;
		fclose(f);
	}
	return r;
}

static double temp_c(void)
{
	FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
	long t;
	if (f && fscanf(f, "%ld", &t) == 1) {
		fclose(f);
		return t / 1000.0;
	}
	if (f)
		fclose(f);
	uint32_t v[2] = { 0, 0 };
	if (mbox(TAG_GET_TEMPERATURE, v, 1, 2) == 0)
		return v[1] / 1000.0;
	return -1;
}

static void col(char *s, size_t n, long v)
{
	if (v < 0)
		snprintf(s, n, "-");
	else
		snprintf(s, n, "%d", (int)v);
}

int main(int argc, char **argv)
{
	double secs = argc > 1 ? atof(argv[1]) : 60;
	int ms = argc > 2 ? atoi(argv[2]) : 1000;
	if (ms < 100)
		ms = 100;
	setvbuf(stdout, NULL, _IOLBF, 0);

	mbox_fd = open("/dev/vcio", O_RDWR);
	if (mbox_fd < 0)
		printf("# mailbox: none (/dev/vcio: %s)\n", strerror(errno));
	else if (clock_mhz(3) < 0) {
		printf("# mailbox: /dev/vcio opened but the clock query failed\n");
		close(mbox_fd);
		mbox_fd = -1;
	} else
		printf("# mailbox: ok\n");
	long th = throttled();
	if (th >= 0)
		printf("# throttle flags since boot: 0x%lx\n", (unsigned long)th);
	pmu_open();
	printf("#     mono  loop   pmu   arm core  v3d sdram    thr   temp\n");

	double start = now_s(CLOCK_MONOTONIC), next = start;
	while (now_s(CLOCK_MONOTONIC) - start < secs) {
		double loop, pmu;
		measure(&loop, &pmu);
		char a[16], c[16], v[16], s[16], p[16], t[16];
		col(a, sizeof a, clock_mhz(3));
		col(c, sizeof c, clock_mhz(4));
		col(v, sizeof v, clock_mhz(5));
		col(s, sizeof s, clock_mhz(8));
		col(p, sizeof p, pmu_fd >= 0 ? (long)(pmu + 0.5) : -1);
		th = throttled();
		if (th < 0)
			snprintf(t, sizeof t, "-");
		else
			snprintf(t, sizeof t, "0x%x", (unsigned)th);
		printf("%10.1f %5.0f %5s %5s %4s %4s %5s %6s %5.1fC\n",
		       now_s(CLOCK_MONOTONIC), loop, p, a, c, v, s, t, temp_c());
		next += ms / 1000.0;
		double w = next - now_s(CLOCK_MONOTONIC);
		if (w > 0) {
			struct timespec ts = { (time_t)w, (long)((w - (time_t)w) * 1e9) };
			nanosleep(&ts, NULL);
		}
	}
	return 0;
}
