#ifndef BENCH_MT_H
#define BENCH_MT_H

/* bench/mt.h —— 多线程并行访存基础设施(test.md §4.16/§4.17,C15/C16)
 *
 * 原则:内核(kernel_defs.h)保持单线程逻辑,线程化只发生在本层 ——
 * 每个工作线程执行同一内核实例,仅地址列表/基址参数按线程错开(§4.16)。
 * 计量:每线程独立 perf 组(pmu_open_pair:cycles+ins,pid=0 = 本线程),
 * 线程内 reset/start → barrier → 跑内核 → stop/read;主线程
 * CLOCK_MONOTONIC 测聚合墙钟(带宽)。PMU 区间 ⊇ 墙钟区间(保守上界)。
 */

#include <stdint.h>
#include <stddef.h>
#include "kernels.h"

/* 单线程工作描述:该线程要执行的内核实例与参数 */
struct mt_job {
    kern_fn k;               /* 内核实例(direct 或 conv 之一) */
    const uint64_t *addr;    /* 该线程地址列表 / 基址流(addr[0]) */
    size_t n;                /* 迭代数 */
    uint64_t p1, p2;
};

/* 一轮并行执行的聚合结果(主线程侧收集) */
#define MT_MAX_THREADS 256
struct mt_stats {
    double cyc_sum;          /* Σ线程 cycles(仅 PMU 可用的线程) */
    double ins_sum;
    double cyc_max;          /* 最慢线程 cycles(尾部) */
    double cyc_min;          /* 最快线程 cycles */
    int    cyc_ok_n;         /* PMU 可用的线程数 */
    double wall_ms;          /* 聚合墙钟(ms,主线程 clock_gettime) */
    /* per-thread 视图(指向 mt.c 内部数组,下次 mt_round 前有效) */
    const double *cyc_thr;
    const double *ins_thr;
    const int    *ok_thr;    /* 每线程 PMU 可用性(1/0) */
};

/* 线程组:创建 T 个 worker(绑核 + 每线程 PMU 对)。
 * 返回实际线程数(≤ 请求值);T<=1 退化为主线程原地执行(仍走 mt_round)。 */
int  mt_init(int T);
void mt_exit(void);

/* 执行一轮并行内核(jobs[t] 与线程 t 绑定,T = mt_init 的返回值)。
 * 三 barrier 相位:arm(PMU 就位)→ go(主线程测 t0,线程开跑)→ done(线程已停表)。 */
void mt_round(const struct mt_job *jobs, int T, struct mt_stats *st);

#endif /* BENCH_MT_H */
