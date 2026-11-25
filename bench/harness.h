#ifndef BENCH_HARNESS_H
#define BENCH_HARNESS_H

/* bench/harness.h —— 计时封装、PMU、缓存/TLB 控制、统计(test.md §3) */

#include <stdint.h>
#include <stddef.h>

/* ---------------- PMU(perf_event_open 组 + 逐 fd 单独读,test.md §3.1/§3.2) ---------------- */

enum pmu_metric {
    PMU_CYC,      /* cycles                     */
    PMU_INS,      /* instructions               */
    PMU_L1M,      /* L1-dcache load misses      */
    PMU_LLCM,     /* LLC load misses            */
    PMU_DTLB,     /* dTLB load misses           */
    PMU_L2M,      /* L2 miss(raw,x86 默认关;ARM raw 0x17 L2D_CACHE_REFILL)*/
    PMU_NMET
};

struct pmuset {
    int fd[PMU_NMET];
    uint64_t id[PMU_NMET];
    uint64_t val[PMU_NMET];    /* pmu_read() 后有效;缩放校正后 */
    int n;                     /* 成功打开的事件数 */
    int ok;                    /* 1 = perf 可用,0 = 回退计时 */
    int scaled;                /* 1 = 发生多路复用(time_enabled != time_running) */
};

/* want_l2m:是否尝试打开 L2 miss;raw_l2m:<0 关闭,否则 raw config。
 * x86 默认 -1(Intel raw umask 因微架构而异,可用 --raw-l2m 指定,如 0x2D1);
 * AArch64 默认 0x17(PMUv3 架构化 L2D_CACHE_REFILL)。 */
void pmu_open(struct pmuset *s, int want_l2m, long raw_l2m);
void pmu_close(struct pmuset *s);
void pmu_reset(struct pmuset *s);
void pmu_start(struct pmuset *s);   /* ENABLE 组 */
void pmu_stop(struct pmuset *s);    /* DISABLE 组 */
void pmu_read(struct pmuset *s);    /* 逐 fd 单独读 + 缩放校正,结果在 s->val[] */

/* 回退计时:x86 = lfence;rdtsc(串行化,墙钟交叉校验);AArch64 = cntvct_el0
 * (固定频率架构定时器,不是 CPU cycles,仅作墙钟回退,test.md §3.1)。 */
static inline uint64_t tsc_now(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile ("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

/* ---------------- 缓存 / TLB 状态控制(test.md §3.4) ---------------- */

/* evict_llc 用的清扫缓冲(大小 = llc_mb MB,默认 96);flush 模式:auto/cl/evict */
void cache_ctl_init(long llc_mb, int flush_cl /* 1=clflush/dc civac, 0=evict */);
void table_warm(size_t entries);          /* 顺序触碰表前 entries 项(预热) */
void table_flush(size_t entries);         /* clflush / dc civac 表已用区间;无权限回退 evict */
void evict_llc(void);                     /* 流式清扫 3×LLC 量级缓冲(通用回退) */
void data_warm(const void *p, size_t bytes);  /* 写触碰数据区(真实分配页 + 预热) */
void data_cold(void);                     /* = evict_llc */

/* ---------------- 统计(test.md §3.6):中位数 ± MAD ---------------- */

double median_of(const double *v, size_t n);
double mad_of(const double *v, size_t n, double med);

/* ---------------- 杂项 ---------------- */

uint64_t xs64(uint64_t *state);           /* xorshift64*,固定种子预生成(§3.5) */
double   cpu_freq_mhz(void);              /* 实测频率记录(§7),失败返回 0 */
int      current_cpu(void);               /* sched_getcpu */

#endif /* BENCH_HARNESS_H */
