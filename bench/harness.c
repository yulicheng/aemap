/* bench/harness.c —— 计时/PMU/缓存控制/统计实现(test.md §3)
 *
 * PMU:perf_event_open 组(cycles 领队)+ PERF_FORMAT_GROUP 一次 read() 读全组,
 * 用 time_enabled/time_running 做多路复用缩放校正;用户态计数(exclude_kernel=1)。
 * 内核单次 ~ms 量级,read() 开销可忽略;无 perf 环境回退 rdtsc/cntvct 墙钟。
 *
 * 缓存控制:表 flush 用 clflush(x86)/ dc civac(AArch64,启动时 SIGILL 探测
 * SCTLR.UCI 权限),失败或被要求时回退流式 evict_llc(§3.4/§7)。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>   /* perf_event_attr / PERF_FORMAT_GROUP 等 */

#include "vva.h"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

/* ============================ PMU ============================ */

struct pmu_attr_spec { int met; uint32_t type; uint64_t cfg; };

static long sys_perf_event_open(struct perf_event_attr *a, pid_t pid, int cpu, int gfd, unsigned long flags) {
    return syscall(__NR_perf_event_open, a, pid, cpu, gfd, flags);
}

static void attr_init(struct perf_event_attr *a, uint32_t type, uint64_t cfg) {
    memset(a, 0, sizeof(*a));
    a->size = sizeof(*a);
    a->type = type;
    a->config = cfg;
    a->disabled = 1;
    a->exclude_kernel = 1;   /* 内核只跑用户态 */
    a->exclude_hv = 1;
    a->read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID
                   | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
}

#define L1D_MISS_CFG  ((PERF_COUNT_HW_CACHE_L1D  << 16) | (PERF_COUNT_HW_CACHE_OP_READ  << 8) | PERF_COUNT_HW_CACHE_RESULT_MISS)
#define LLC_MISS_CFG  ((PERF_COUNT_HW_CACHE_LL  << 16) | (PERF_COUNT_HW_CACHE_OP_READ  << 8) | PERF_COUNT_HW_CACHE_RESULT_MISS)
#define DTLB_MISS_CFG ((PERF_COUNT_HW_CACHE_DTLB << 16) | (PERF_COUNT_HW_CACHE_OP_READ  << 8) | PERF_COUNT_HW_CACHE_RESULT_MISS)

void pmu_open(struct pmuset *s, int want_l2m, long raw_l2m) {
    memset(s, 0, sizeof(*s));

    struct pmu_attr_spec list[PMU_NMET];
    int m = 0;
    list[m++] = (struct pmu_attr_spec){ .met = PMU_CYC,  .type = PERF_TYPE_HARDWARE, .cfg = PERF_COUNT_HW_CPU_CYCLES };
    list[m++] = (struct pmu_attr_spec){ .met = PMU_INS,  .type = PERF_TYPE_HARDWARE, .cfg = PERF_COUNT_HW_INSTRUCTIONS };
    list[m++] = (struct pmu_attr_spec){ .met = PMU_L1M,  .type = PERF_TYPE_HW_CACHE, .cfg = L1D_MISS_CFG };
    list[m++] = (struct pmu_attr_spec){ .met = PMU_LLCM, .type = PERF_TYPE_HW_CACHE, .cfg = LLC_MISS_CFG };
    list[m++] = (struct pmu_attr_spec){ .met = PMU_DTLB, .type = PERF_TYPE_HW_CACHE, .cfg = DTLB_MISS_CFG };
    if (want_l2m && raw_l2m >= 0)
        list[m++] = (struct pmu_attr_spec){ .met = PMU_L2M, .type = PERF_TYPE_RAW, .cfg = (uint64_t)raw_l2m };

    int leader = -1;
    for (int i = 0; i < m; i++) {
        struct perf_event_attr a;
        attr_init(&a, list[i].type, list[i].cfg);
        int fd = (int)sys_perf_event_open(&a, 0, -1, leader, 0);
        if (fd < 0) {
            if (leader < 0) return;        /* cycles 打不开 → perf 整体不可用 */
            continue;                       /* 个别事件缺,跳过 */
        }
        if (leader < 0) leader = fd;
        s->fd[list[i].met] = fd;
        ioctl(fd, PERF_EVENT_IOC_ID, &s->id[list[i].met]);
        s->n++;
    }
    s->ok = (leader >= 0);
}

void pmu_close(struct pmuset *s) {
    for (int i = 0; i < PMU_NMET; i++)
        if (s->fd[i] >= 0) { close(s->fd[i]); s->fd[i] = -1; }
    s->n = 0; s->ok = 0;
}

static void group_ioctl(struct pmuset *s, int cmd, unsigned long arg) {
    for (int i = 0; i < PMU_NMET; i++)
        if (s->fd[i] >= 0) ioctl(s->fd[i], cmd, arg);
}

void pmu_reset(struct pmuset *s) { group_ioctl(s, PERF_EVENT_IOC_RESET, 0); }
void pmu_start(struct pmuset *s) { group_ioctl(s, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP); }
void pmu_stop (struct pmuset *s) { group_ioctl(s, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP); }

void pmu_read(struct pmuset *s) {
    for (int i = 0; i < PMU_NMET; i++) s->val[i] = 0;
    if (!s->ok) return;

    uint64_t buf[4 + 2 * PMU_NMET];
    int leader = s->fd[PMU_CYC] >= 0 ? s->fd[PMU_CYC] : s->fd[PMU_INS];
    ssize_t nr = read(leader, buf, sizeof(buf));
    if (nr < 8) return;

    uint64_t cnt = buf[0];
    if ((size_t)nr < (1 + 2 * cnt) * 8) return;
    double ena = 0, run = 0;
    if ((size_t)nr >= (1 + 2 * cnt + 2) * 8) {
        ena = (double)buf[1 + 2 * cnt];
        run = (double)buf[1 + 2 * cnt + 1];
    }
    double scale = (ena > 0 && run > 0) ? ena / run : 1.0;
    s->scaled = (ena > 0 && run > 0 && run < ena * 0.99);

    for (uint64_t k = 0; k < cnt; k++) {
        uint64_t v = buf[1 + 2 * k], id = buf[2 + 2 * k];
        for (int i = 0; i < PMU_NMET; i++)
            if (s->fd[i] >= 0 && s->id[i] == id) {
                s->val[i] = (uint64_t)((double)v * scale);   /* 多路复用校正(§7) */
                break;
            }
    }
}

/* ==================== 缓存 / TLB 状态控制 ==================== */

static uint8_t *g_evbuf;
static size_t   g_evsize;
static int      g_flush_cl;    /* 1 = 用 clflush / dc civac */

#if defined(__aarch64__)
static sigjmp_buf g_dc_jb;
static volatile sig_atomic_t g_dc_ok = -1;
static void dc_sigill(int sig) { (void)sig; siglongjmp(g_dc_jb, 1); }

static int dc_supported(void) {
    if (g_dc_ok >= 0) return g_dc_ok;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = dc_sigill;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, &old);
    volatile uint64_t probe = 1;
    if (sigsetjmp(g_dc_jb, 1) == 0) {
        __asm__ volatile ("dc civac, %0" :: "r"((uintptr_t)&probe) : "memory");
        g_dc_ok = 1;
    } else {
        g_dc_ok = 0;   /* SCTLR.UCI 未开放(§7),回退 evict_llc */
    }
    sigaction(SIGILL, &old, NULL);
    return g_dc_ok;
}
#endif

void cache_ctl_init(long llc_mb, int flush_cl) {
    if (g_evbuf) { munmap(g_evbuf, g_evsize); g_evbuf = NULL; }
    if (llc_mb <= 0) llc_mb = 96;
    g_evsize = (size_t)llc_mb << 20;
    g_evbuf = mmap(NULL, g_evsize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_POPULATE, -1, 0);
    if (g_evbuf == MAP_FAILED) { g_evbuf = NULL; g_evsize = 0; }
    g_flush_cl = flush_cl;
}

void evict_llc(void) {
    if (!g_evbuf) return;
    volatile uint8_t *p = g_evbuf;
    for (size_t i = 0; i < g_evsize; i += 64)
        p[i] ^= 0xA5;              /* 写触碰:逐行 write-allocate,驱逐 LRU(§3.4) */
}

void table_warm(size_t entries) {
    /* 顺序触碰前 entries 项;表基址来自 vva_table_base(),编码无关 */
    const volatile uint8_t *t = vva_table_base();
    size_t bytes = entries * vva_table_entry_bytes();
    for (size_t i = 0; i < bytes; i += 64)
        (void)t[i];
}

static void flush_range_cl(volatile uint8_t *p, size_t bytes) {
#if defined(__x86_64__)
    for (size_t i = 0; i < bytes; i += 64)
        _mm_clflush((const void *)(p + i));
    __asm__ volatile ("mfence" ::: "memory");
#elif defined(__aarch64__)
    if (!dc_supported()) { g_flush_cl = 0; return; }
    for (size_t i = 0; i < bytes; i += 64)
        __asm__ volatile ("dc civac, %0" :: "r"((uintptr_t)(p + i)) : "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
#else
    (void)p; (void)bytes;
#endif
}

void table_flush(size_t entries) {
    size_t bytes = (entries * vva_table_entry_bytes() + 63) & ~(size_t)63;
    volatile uint8_t *t = vva_table_base();
    if (g_flush_cl) {
        flush_range_cl(t, bytes);
        return;
    }
    evict_llc();                   /* 无 clflush 权限时的通用回退(§3.4) */
}

void data_warm(const void *p, size_t bytes) {
    /* 写触碰:既保证匿名页真实分配(避免共享零页),又把行预取进缓存 */
    volatile uint8_t *q = (volatile uint8_t *)p;
    for (size_t i = 0; i < bytes; i += 64)
        q[i] = q[i];
}

void data_cold(void) { evict_llc(); }

/* ============================ 统计 ============================ */

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

double median_of(const double *v, size_t n) {
    if (!n) return 0.0;
    double *tmp = malloc(n * sizeof(double));
    memcpy(tmp, v, n * sizeof(double));
    qsort(tmp, n, sizeof(double), cmp_double);
    double med = (n & 1) ? tmp[n / 2] : (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0;
    free(tmp);
    return med;
}

double mad_of(const double *v, size_t n, double med) {
    if (!n) return 0.0;
    double *tmp = malloc(n * sizeof(double));
    for (size_t i = 0; i < n; i++)
        tmp[i] = v[i] > med ? v[i] - med : med - v[i];
    double mad = median_of(tmp, n);
    free(tmp);
    return mad;
}

/* ============================ 杂项 ============================ */

uint64_t xs64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *state = x;
    return x * 2685821657736338717ULL;
}

int current_cpu(void) { return sched_getcpu(); }

double cpu_freq_mhz(void) {
    int cpu = sched_getcpu();
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    FILE *f = fopen(path, "r");
    if (f) {
        long khz = 0;
        if (fscanf(f, "%ld", &khz) == 1 && khz > 0) { fclose(f); return khz / 1000.0; }
        fclose(f);
    }
    /* 回退:/proc/cpuinfo 的 "cpu MHz"(虚拟机常无 cpufreq) */
    f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0.0;
    char line[256];
    double best = 0;
    int in_target = (cpu < 0);
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "processor", 9)) {
            int p = atoi(strchr(line, ':') ? strchr(line, ':') + 1 : "0");
            in_target = (cpu < 0 || p == cpu);
        } else if (in_target && sscanf(line, "cpu MHz : %lf", &best) == 1) {
            break;
        }
    }
    fclose(f);
    return best;
}
