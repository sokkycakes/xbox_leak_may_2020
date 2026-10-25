/* Runs a benchmark REPS times, so one run takes long enough to time on a Pi. */
int bench_once(void);
int bench(void)
{
    unsigned r = 0;
    for (int i = 0; i < REPS; i++) r = r * 31u + (unsigned)bench_once();
    return (int)r;
}
