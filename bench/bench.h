/*
 * bench.h — VVA→VA overhead 测试框架公共定义（test.md §3）。
 *   harness（本文件 + main.c）/ patterns / timer / report / runtime 控制。
 */
#ifndef VAMAP_BENCH_H
#define VAMAP_BENCH_H

#include "vamap.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* timer：rdtsc(fenced) 计时 + wall clock + perf_event 封装（§9.1）      */
/* ------------------------------------------------------------------ */

uint64_t bench_wall_ns(void);
uint64_t bench_tsc(void);            /* 序列化 rdtsc（x86: lfence;rdtsc） */
double   bench_tsc_hz(void);         /* 借用运行时标定结果 */

enum {
    PC_CYCLES = 0, PC_INSTR, PC_L1M, PC_LLC, PC_DTLB, PC_BR, PC_N
};
typedef struct {
    int fd[PC_N];
    int ok[PC_N];
    uint64_t base[PC_N];
    int any;
} bench_perf;

int      bench_perf_open(bench_perf *p);    /* 失败自动降级（§9.1） */
void     bench_perf_close(bench_perf *p);
void     bench_perf_start(bench_perf *p);
void     bench_perf_stop(bench_perf *p);
int      bench_perf_available(void);

/* ------------------------------------------------------------------ */
/* 运行配置                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    double   invalid_ratio;     /* 0/0.25/0.5/0.75/1 */
    uint64_t latency_ns;       /* 10us/100us/1ms/10ms */
    int      latency_mode;     /* VAMAP_LATENCY_SPIN / _SLEEP */
    int      alias;            /* 0/1 → alias 池 64 */
    int      scope_rand;       /* 1=rand 0=target */
    uint64_t ops;              /* 每线程 ld/st 次数 */
    int      reps;
    uint64_t max_creates;      /* §4.4 每重复 create 预算 */
    int      nthreads;
    int      idx_shared;       /* 1=shared 0=private（§8 索引共享模式） */
    /* pattern 变体参数 */
    uint64_t var_u;            /* T1: W 字节 / T7: S / T9: s×1000 / T12: K /
                                  T8: 槽位数 / T10: W / STREAM-M: 分块数 */
    uint64_t var_u2;           /* STREAM-M: 分块字节上限 / T14: 数据窗口 / T2: DATA_WINDOW */
    const char *variant;       /* 变体名记录 */
    int      npin;             /* 绑核列表长度（0=不绑核） */
    const int *pin;            /* 绑核 CPU 列表 */
    const char *suite;         /* CSV suite 字段（single/matrix） */
} run_params;

/* 前置声明：pattern_ops 的函数指针以 run_ctx 为参数 */
typedef struct run_ctx run_ctx;

/* pattern 操作表（先于 run_ctx 定义，供其引用） */
typedef struct {
    const char *name;
    int   id;
    /* 每配置一次：建 runtime、replay 出 touched/va_of_idx、初始状态 */
    int (*setup)(run_ctx *rc);
    /* 每 rep 每阶段：diff 到目标状态（phase 1: 全 touched 有效；2: V），
     * 清零 stats */
    void (*reset_phase)(run_ctx *rc, int phase);
    /* 预热：提交目标状态有效表项对应的数据页（计时外） */
    void (*warm)(run_ctx *rc, int phase);
    /* 内核：mode 0=B0 1=B1 2=B2；tid/nthreads 线程布局 */
    void (*kernel)(run_ctx *rc, int mode, int tid, int nthreads);
    /* 每 rep 预期 creates（§9.3 一致性校验） */
    uint64_t (*expected_creates)(run_ctx *rc, int phase);
    void (*fini)(run_ctx *rc);
    /* 该 pattern 需要的 alias 池大小（0 = 按 --alias） */
    uint32_t pool_need;
} pattern_ops;

/* 单次内核测量结果 */
typedef struct {
    uint64_t tsc;
    uint64_t wall;
    uint64_t ops;
    uint64_t creates, destroys, spin_ns, slow_ns_total, slow_calls;
    long     minflt, majflt;
    uint64_t perf[PC_N];
} kern_meas;

/* 运行上下文：一个（pattern × 配置）的全部状态 */
typedef struct run_ctx {
    run_params p;
    int        pattern_id;
    const char *pattern_name;
    const pattern_ops *pat;

    /* 模式采样（B0/B1/B2） */
    kern_meas  samples[3][64]; /* 按 rep */
    int        nsamples[3];

    /* 映射状态（setup 填写） */
    uint32_t   M;              /* 可触及表项空间（§4.4 缩规模后） */
    uint64_t  *va_of_idx;      /* [M] 每 idx 的 VA 基址（B0 用） */
    uint8_t   *touched;        /* [M] 本序列触及的 idx */
    uint8_t   *invalid;        /* [M] 初始无效集合（∩ touched） */
    uint8_t   *cur_valid;      // runtime 表状态镜像
    uint32_t   n_touched;
    uint32_t   n_invalid;
    uint64_t   seq_seed;       /* 固定序列种子（各 rep 相同，保证可复现） */

    /* pattern 私有数据 */
    void      *priv;

    /* 内核运行期（线程间） */
    volatile uint64_t ops_actual;
    volatile uint64_t sink;
    uint64_t   t_start, t_end; /* 全体线程 max(end)-min(start) */
    volatile uint64_t w_start, w_end;   /* 墙钟极值（ns） */
    pthread_barrier_t bar;
    int        affinity_n;
    int        affinity[256];
    bench_perf perf_main;
} run_ctx;

/* patterns（T1–T15，test.md §6）：操作表定义见上方 pattern_ops          */

const pattern_ops *pattern_by_id(int id);
int pattern_count(void);
const pattern_ops *pattern_at(int i);

/* patterns.c 提供的公共小工具 */
uint64_t xs64(uint64_t *s);
uint32_t pow2_floor(uint32_t v);
void    *bench_xmalloc(size_t n);

/* 状态工具（patterns.c 实现，main/correctness 亦可用） */
void bench_diff_state_target(run_ctx *rc, const uint8_t *target_valid);
void bench_build_invalid(run_ctx *rc);       /* 按 scope 选 invalid 集合 */
void bench_free_state(run_ctx *rc);          /* 释放 va_of_idx/touched/invalid/cur_valid */

/* ------------------------------------------------------------------ */
/* harness / report（main.c）                                          */
/* ------------------------------------------------------------------ */

int  bench_run_config(const pattern_ops *pat, run_params *p, FILE *csv,
                      const char *build_tag);
void bench_csv_header(FILE *f);
void bench_emit_row(FILE *f, const char *build_tag, const pattern_ops *pat,
                    run_ctx *rc, int mode, const kern_meas *ms,
                    double ns_med, double ns_min, double ns_max,
                    double overhead_pct, double cv, const char *notes);

int  bench_parse_cpulist(const char *s, int *out, int cap);
void bench_get_thread_faults(long *minflt, long *majflt);

/* correctness 套件（correctness.c）：返回失败用例数 */
int bench_run_correctness(void);

#endif /* VAMAP_BENCH_H */
