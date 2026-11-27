/*
 * timer.c — rdtsc(fenced) 计时 + wall clock + perf_event_open 封装（test.md §9.1）。
 * perf 权限受限时自动降级为纯 rdtsc 并由调用方标注（§9.2）。
 */
#define _GNU_SOURCE
#include "bench.h"

#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__)
uint64_t bench_tsc(void)
{
    uint32_t a, d;
    __asm__ volatile ("lfence; rdtsc" : "=a"(a), "=d"(d));
    return (uint64_t)a | ((uint64_t)d << 32);
}
#elif defined(__aarch64__)
uint64_t bench_tsc(void)
{
    uint64_t v;
    __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
#else
uint64_t bench_tsc(void) { return bench_wall_ns(); }
#endif

uint64_t bench_wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

double bench_tsc_hz(void)
{
    double h = vamap_tsc_hz();
    return h > 0 ? h : 1e9;
}

void bench_get_thread_faults(long *minflt, long *majflt)
{
    struct rusage ru;
    getrusage(RUSAGE_THREAD, &ru);
    *minflt = ru.ru_minflt;
    *majflt = ru.ru_majflt;
}

/* ------------------------------------------------------------------ */
/* perf_event_open：cycles/instructions/L1-miss/LLC-miss/dTLB-miss/br   */
/* ------------------------------------------------------------------ */

#ifdef __linux__
#include <linux/perf_event.h>

static int perf_open_one(struct perf_event_attr *attr)
{
    int fd = (int)syscall(SYS_perf_event_open, attr, 0, -1, -1, 0);
    return fd;
}

static void attr_init(struct perf_event_attr *a, uint32_t type, uint64_t cfg)
{
    memset(a, 0, sizeof(*a));
    a->type = type;
    a->size = sizeof(*a);
    a->config = cfg;
    a->disabled = 1;
    a->exclude_kernel = 1;
    a->exclude_hv = 1;
    /* 事件多于硬件计数器时被多路复用：读数须按 enabled/running 缩放 */
    a->read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
                     PERF_FORMAT_TOTAL_TIME_RUNNING;
}

int bench_perf_open(bench_perf *p)
{
    static const struct { uint32_t type; uint64_t cfg; } ev[PC_N] = {
        [PC_CYCLES] = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES },
        [PC_INSTR]  = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS },
        [PC_L1M]    = { PERF_TYPE_HW_CACHE,
                        PERF_COUNT_HW_CACHE_L1D |
                        (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                        (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) },
        [PC_LLC]    = { PERF_TYPE_HW_CACHE,
                        PERF_COUNT_HW_CACHE_LL |
                        (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                        (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) },
        [PC_DTLB]   = { PERF_TYPE_HW_CACHE,
                        PERF_COUNT_HW_CACHE_DTLB |
                        (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                        (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) },
        [PC_BR]     = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES },
    };
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < PC_N; i++) {
        struct perf_event_attr a;
        attr_init(&a, ev[i].type, ev[i].cfg);
        p->fd[i] = perf_open_one(&a);
        p->ok[i] = (p->fd[i] >= 0);
        if (p->ok[i])
            p->any = 1;
    }
    return p->any ? 0 : -1;
}

void bench_perf_close(bench_perf *p)
{
    for (int i = 0; i < PC_N; i++)
        if (p->fd[i] >= 0) {
            close(p->fd[i]);
            p->fd[i] = -1;
        }
    p->any = 0;
}

void bench_perf_start(bench_perf *p)
{
    for (int i = 0; i < PC_N; i++) {
        if (!p->ok[i])
            continue;
        ioctl(p->fd[i], PERF_EVENT_IOC_RESET, 0);
        ioctl(p->fd[i], PERF_EVENT_IOC_ENABLE, 0);
    }
}

void bench_perf_stop(bench_perf *p)
{
    for (int i = 0; i < PC_N; i++) {
        if (!p->ok[i])
            continue;
        ioctl(p->fd[i], PERF_EVENT_IOC_DISABLE, 0);
        /* 读格式：[count, time_enabled, time_running]；复用时按比例放大
         * （enabled≥running，running=0 则丢弃样本） */
        uint64_t v[3] = { 0, 0, 0 };
        if (read(p->fd[i], v, sizeof(v)) == (ssize_t)sizeof(v)) {
            p->base[i] = (v[2] && v[2] < v[1])
                             ? (uint64_t)((double)v[0] * (double)v[1] /
                                          (double)v[2])
                             : v[0];
        }
    }
}

int bench_perf_available(void)
{
    bench_perf p;
    if (bench_perf_open(&p) != 0)
        return 0;
    int ok = p.ok[PC_CYCLES] && p.ok[PC_INSTR];
    bench_perf_close(&p);
    return ok;
}

#else /* !__linux__ */

int bench_perf_open(bench_perf *p) { memset(p, 0, sizeof(*p)); return -1; }
void bench_perf_close(bench_perf *p) { (void)p; }
void bench_perf_start(bench_perf *p) { (void)p; }
void bench_perf_stop(bench_perf *p) { (void)p; }
int bench_perf_available(void) { return 0; }

#endif
