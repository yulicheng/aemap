/*
 * main.c — vamap_bench CLI（test.md §3/§4.1/§7/§11）。
 *   --suite correctness | --test T<n> [参数…] | --matrix --level smoke|standard|full
 * 负责：参数解析、矩阵生成、重复调度（自适应加跑 §7.5）、基线交错（B0 与
 * B1/B2 同 rep 相邻执行）、CSV/汇总输出。
 */
#define _GNU_SOURCE
#include "bench.h"

#include <math.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#define MAX_REPS_CAP 20
#define MAX_THREADS  64

static int         g_pin[256];
static int         g_npin;
static const char *g_suite = "single";
static char        g_clksrc[32] = "tsc";   /* 内核时钟源（ns 窗口基准的参照） */

/* ------------------------------------------------------------------ */
/* 参数解析                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int      suite_correctness;
    int      test_id;
    int      matrix;
    int      level;              /* 1=smoke 2=standard 3=full */
    int      th_list[8];
    int      nth;
    int      nth_set;
    int      idx_shared;
    double   invalid;
    int      scope_rand;
    uint64_t latency_ns;
    int      latency_mode;
    int      alias;
    uint64_t ops;
    int      reps;
    uint64_t max_creates;
    uint64_t var_u, var_u2;
    const char *variant;
    const char *out;
    int      list;
} opts_t;

static uint64_t parse_time_ns(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    if (!strcmp(end, "us") || !strcmp(end, "µs"))
        return (uint64_t)(v * 1000.0);
    if (!strcmp(end, "ms"))
        return (uint64_t)(v * 1e6);
    if (!strcmp(end, "s"))
        return (uint64_t)(v * 1e9);
    return (uint64_t)v;                     /* ns */
}

static int parse_int_list(const char *s, int *out, int cap)
{
    int n = 0;
    const char *p = s;
    while (*p && n < cap) {
        long v = strtol(p, (char **)&p, 10);
        out[n++] = (int)v;
        if (*p == ',')
            p++;
        else
            break;
    }
    return n;
}

int bench_parse_cpulist(const char *s, int *out, int cap)
{
    int n = 0;
    const char *p = s;
    while (*p && n < cap) {
        long lo = strtol(p, (char **)&p, 10);
        long hi = lo;
        if (*p == '-') {
            p++;
            hi = strtol(p, (char **)&p, 10);
        }
        for (long c = lo; c <= hi && n < cap; c++)
            out[n++] = (int)c;
        if (*p == ',')
            p++;
        else
            break;
    }
    return n;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: vamap_bench --suite correctness\n"
        "       vamap_bench --test T<n> [--threads N[,N...]] [--idx-mode private|shared]\n"
        "           [--invalid R] [--invalid-scope rand|target] [--latency 10us|100us|1ms|10ms]\n"
        "           [--latency-mode spin|sleep] [--alias 0|1] [--ops N] [--reps N]\n"
        "           [--max-creates-per-rep C] [--pin 0-7] [--var-u N] [--var-u2 N]\n"
        "           [--variant NAME] [--out FILE]\n"
        "       vamap_bench --matrix --level smoke|standard|full [--out DIR] [--threads 1,4]\n"
        "       vamap_bench --list\n");
}

/* ------------------------------------------------------------------ */
/* 底层测量（§7/§8）：屏障 + tsc/wall 极值 + perf + faults               */
/* ------------------------------------------------------------------ */

typedef struct {
    run_ctx  *rc;
    int       tid, mode;
    uint64_t  perf[PC_N];
    long      minflt, majflt;
    int       perf_ok;
} warg_t;

static void pin_to_cpu(int cpu)
{
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
}

static inline void atomic_min_u64(volatile uint64_t *p, uint64_t v)
{
    uint64_t cur = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (v < cur &&
           !__atomic_compare_exchange_n(p, &cur, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

static inline void atomic_max_u64(volatile uint64_t *p, uint64_t v)
{
    uint64_t cur = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (v > cur &&
           !__atomic_compare_exchange_n(p, &cur, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

static void *worker_fn(void *arg)
{
    warg_t *w = arg;
    run_ctx *rc = w->rc;
    if (rc->affinity_n > 0)
        pin_to_cpu(rc->affinity[w->tid % rc->affinity_n]);
    bench_perf perf;
    int pok = bench_perf_open(&perf) == 0;
    long mf0 = 0, mm0 = 0, mf1 = 0, mm1 = 0;
    bench_get_thread_faults(&mf0, &mm0);

    pthread_barrier_wait(&rc->bar);
    if (pok)
        bench_perf_start(&perf);
    atomic_min_u64(&rc->t_start, bench_tsc());
    atomic_min_u64(&rc->w_start, bench_wall_ns());
    rc->pat->kernel(rc, w->mode, w->tid, rc->p.nthreads);
    atomic_max_u64(&rc->t_end, bench_tsc());
    atomic_max_u64(&rc->w_end, bench_wall_ns());
    if (pok)
        bench_perf_stop(&perf);
    bench_get_thread_faults(&mf1, &mm1);

    w->minflt = mf1 - mf0;
    w->majflt = mm1 - mm0;
    w->perf_ok = pok;
    if (pok) {
        for (int i = 0; i < PC_N; i++)
            w->perf[i] = perf.base[i];
        bench_perf_close(&perf);
    }
    return NULL;
}

/* 一次内核测量（单模式）：由 run_rep 调用，样本写入 rc->samples[mode] */
static void measure(run_ctx *rc, int mode)
{
    kern_meas *m = &rc->samples[mode][rc->nsamples[mode]++];
    memset(m, 0, sizeof(*m));
    rc->ops_actual = 0;
    rc->sink = 0;
    rc->t_start = UINT64_MAX;
    rc->t_end = 0;
    rc->w_start = UINT64_MAX;
    rc->w_end = 0;
    vamap_reset_stats();

    int nth = rc->p.nthreads;
    if (nth <= 1) {
        warg_t w;
        memset(&w, 0, sizeof(w));
        w.rc = rc;
        w.mode = mode;
        worker_fn(&w);
        m->minflt = w.minflt;
        m->majflt = w.majflt;
        if (w.perf_ok)
            memcpy(m->perf, w.perf, sizeof(m->perf));
    } else {
        warg_t   *ws = bench_xmalloc(sizeof(warg_t) * (size_t)nth);
        pthread_t *th = bench_xmalloc(sizeof(pthread_t) * (size_t)nth);
        for (int t = 0; t < nth; t++) {
            memset(&ws[t], 0, sizeof(ws[t]));
            ws[t].rc = rc;
            ws[t].tid = t;
            ws[t].mode = mode;
            if (pthread_create(&th[t], NULL, worker_fn, &ws[t]) != 0) {
                fprintf(stderr, "bench: pthread_create failed\n");
                exit(2);
            }
        }
        for (int t = 0; t < nth; t++) {
            pthread_join(th[t], NULL);
            m->minflt += ws[t].minflt;
            m->majflt += ws[t].majflt;
            if (ws[t].perf_ok)
                for (int i = 0; i < PC_N; i++)
                    m->perf[i] += ws[t].perf[i];
        }
        free(ws);
        free(th);
    }

    m->tsc = rc->t_end - rc->t_start;
    m->wall = rc->w_end - rc->w_start;
    /* 注：本机 clocksource=hpet 时内核已判定 TSC 跨核不同步（各核 rdtsc
     * 存在数 ms 恒定偏移），多线程极值窗口会被虚增——ns_per_op 一律以
     * 墙钟窗口（CLOCK_MONOTONIC，内核统一时钟源）为准；tsc 窗口仅作记录 */
    m->ops = rc->ops_actual;
    vamap_stats st = vamap_get_stats();
    m->creates = st.creates;
    m->destroys = st.destroys;
    m->spin_ns = st.spin_ns;
    m->slow_ns_total = st.slow_ns_total;
    m->slow_calls = st.slow_calls;
    (void)rc->sink;
}

/* 一次重复：phase1（全 touched 有效 → B0+B1）+ phase2（V 状态 → B0+B2），
 * B0 与被测模式同 rep 相邻执行（基线交错，§7.4） */
static void run_rep(run_ctx *rc)
{
    rc->pat->reset_phase(rc, 1);
    rc->pat->warm(rc, 1);
    measure(rc, 0);
    measure(rc, 1);

    rc->pat->reset_phase(rc, 2);
    rc->pat->warm(rc, 2);
    measure(rc, 0);
    measure(rc, 2);
}

/* ------------------------------------------------------------------ */
/* 统计聚合                                                             */
/* ------------------------------------------------------------------ */

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median_of(double *v, int n)
{
    qsort(v, (size_t)n, sizeof(double), cmp_dbl);
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static double cv_of(const double *v, int n)
{
    if (n < 2)
        return 0.0;
    double mean = 0;
    for (int i = 0; i < n; i++)
        mean += v[i];
    mean /= n;
    if (mean <= 0)
        return 0.0;
    double var = 0;
    for (int i = 0; i < n; i++)
        var += (v[i] - mean) * (v[i] - mean);
    return sqrt(var / n) / mean;
}

static double tsc_to_ns(uint64_t cyc)
{
    return (double)cyc * 1e9 / bench_tsc_hz();
}

/* 窗口时长（ns）：以墙钟窗口为准（见 measure 内注释——clocksource=hpet
 * 时 rdtsc 跨核偏移会虚增多线程窗口） */
static double win_ns(const kern_meas *m)
{
    return m->wall > 0 ? (double)m->wall : tsc_to_ns(m->tsc);
}

static double max_mode_cv(run_ctx *rc)
{
    double worst = 0;
    for (int mode = 0; mode < 3; mode++) {
        int n = rc->nsamples[mode];
        if (n < 2)
            continue;
        double ns[64];
        int m = 0;
        for (int i = 0; i < n; i++)
            if (rc->samples[mode][i].ops)
                ns[m++] = win_ns(&rc->samples[mode][i]) /
                          (double)rc->samples[mode][i].ops;
        if (m >= 2) {
            double c = cv_of(ns, m);
            if (c > worst)
                worst = c;
        }
    }
    return worst;
}

/* ------------------------------------------------------------------ */
/* CSV（§11）                                                           */
/* ------------------------------------------------------------------ */

void bench_csv_header(FILE *f)
{
    fprintf(f,
        "suite,host,date,arch,build,test_id,pattern,variant,mode,threads,idx_mode,alias,"
        "invalid_ratio,invalid_ratio_actual,invalid_scope,latency_ns,latency_mode,"
        "region_count,arena_base,M,ops,reps,creates_expected,creates_actual,destroys,"
        "ns_per_op_med,ns_per_op_min,ns_per_op_max,cycles_per_op,instr_per_op,"
        "l1m_per_op,llc_per_op,dtlb_per_op,br_per_op,minor_faults,major_faults,"
        "slow_ns_total,slow_pct,overhead_pct,CV,notes\n");
}

static void csv_san(char *d, size_t cap, const char *s)
{
    size_t j = 0;
    for (size_t i = 0; s && s[i] && j + 1 < cap; i++)
        d[j++] = (s[i] == ',' || s[i] == '"' || s[i] == '\n') ? ';' : s[i];
    d[j] = 0;
}

void bench_emit_row(FILE *f, const char *build_tag, const pattern_ops *pat,
                    run_ctx *rc, int mode, const kern_meas *ms,
                    double ns_med, double ns_min, double ns_max,
                    double overhead_pct, double cv, const char *notes)
{
    char host[128] = "?", date[40] = "?", arch[32] = "?", note[256];
    csv_san(note, sizeof(note), notes);
    if (gethostname(host, sizeof(host) - 1) != 0)
        snprintf(host, sizeof(host), "unknown");
    time_t t = time(NULL);
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", localtime(&t));
    struct utsname u;
    if (uname(&u) == 0)
        snprintf(arch, sizeof(arch), "%.31s", u.machine);

    vamap_info_t info = vamap_get_info();
    uint64_t exp = (mode == 2) ? pat->expected_creates(rc, 2) : 0;
    double iact = rc->n_touched ? (double)rc->n_invalid / (double)rc->n_touched : 0.0;
    double cyc = ms->ops ? (double)ms->perf[PC_CYCLES] / (double)ms->ops : 0.0;
    double ins = ms->ops ? (double)ms->perf[PC_INSTR] / (double)ms->ops : 0.0;
    double l1m = ms->ops ? (double)ms->perf[PC_L1M] / (double)ms->ops : 0.0;
    double llc = ms->ops ? (double)ms->perf[PC_LLC] / (double)ms->ops : 0.0;
    double dtl = ms->ops ? (double)ms->perf[PC_DTLB] / (double)ms->ops : 0.0;
    double brm = ms->ops ? (double)ms->perf[PC_BR] / (double)ms->ops : 0.0;
    double spct = ms->wall ? (double)ms->slow_ns_total / (double)ms->wall * 100.0 : 0.0;

    fprintf(f,
        "%s,%s,%s,%s,%s,T%d,%s,%s,%s,%d,%s,%d,"
        "%.4f,%.4f,%s,%llu,%s,"
        "%u,0x%llx,%u,%llu,%d,"
        "%llu,%llu,%llu,"
        "%.4f,%.4f,%.4f,%.3f,%.3f,"
        "%.4f,%.4f,%.4f,%.4f,%ld,%ld,"
        "%llu,%.2f,%.2f,%.3f,%s\n",
        rc->p.suite ? rc->p.suite : "single", host, date, arch, build_tag,
        pat->id, pat->name, rc->p.variant ? rc->p.variant : "",
        mode == 0 ? "B0" : (mode == 1 ? "B1" : "B2"),
        rc->p.nthreads, rc->p.idx_shared ? "shared" : "private", rc->p.alias,
        rc->p.invalid_ratio, iact, rc->p.scope_rand ? "rand" : "target",
        (unsigned long long)rc->p.latency_ns,
        rc->p.latency_mode == VAMAP_LATENCY_SLEEP ? "sleep" : "spin",
        info.region_count, (unsigned long long)info.arena_base, rc->M,
        (unsigned long long)rc->p.ops, rc->nsamples[mode],
        (unsigned long long)exp, (unsigned long long)ms->creates,
        (unsigned long long)ms->destroys,
        ns_med, ns_min, ns_max, cyc, ins,
        l1m, llc, dtl, brm, ms->minflt, ms->majflt,
        (unsigned long long)ms->slow_ns_total, spct, overhead_pct, cv, note);
}

/* ------------------------------------------------------------------ */
/* 单配置运行                                                           */
/* ------------------------------------------------------------------ */

int bench_run_config(const pattern_ops *pat, run_params *p, FILE *csv,
                     const char *build_tag)
{
    run_ctx rc;
    memset(&rc, 0, sizeof(rc));
    rc.p = *p;
    rc.pat = pat;
    rc.pattern_id = pat->id;
    rc.pattern_name = pat->name;
    rc.seq_seed = 0x2545F4914F6CDD1DULL;
    rc.affinity_n = p->npin > 0 ? p->npin : 0;
    if (rc.affinity_n)
        memcpy(rc.affinity, p->pin, sizeof(int) * (size_t)rc.affinity_n);
    if (pthread_barrier_init(&rc.bar, NULL,
                             (unsigned)(p->nthreads > 0 ? p->nthreads : 1)) != 0)
        return -1;

    if (pat->setup(&rc) != 0) {
        fprintf(stderr, "bench: setup failed for %s\n", pat->name);
        pthread_barrier_destroy(&rc.bar);
        return -1;
    }

    /* 重复 + 自适应加跑（§7.5：CV > 5% 加跑，上限 20） */
    int reps = 0;
    for (; reps < rc.p.reps && reps < MAX_REPS_CAP; reps++)
        run_rep(&rc);
    while (reps < MAX_REPS_CAP && max_mode_cv(&rc) > 0.05) {
        run_rep(&rc);
        reps++;
    }

    /* 聚合 + 输出 */
    double b0_med = 0;
    int    b0_n = rc.nsamples[0];
    char   notes[128];
    snprintf(notes, sizeof(notes), "%s%s;clk=%s",
             rc.affinity_n ? ";pin" : "",
             bench_perf_available() ? "" : ";perf-unavailable",
             g_clksrc);

    for (int mode = 0; mode < 3; mode++) {
        int n = rc.nsamples[mode];
        if (!n)
            continue;
        double ns[64];
        int m = 0;
        for (int i = 0; i < n; i++)
            if (rc.samples[mode][i].ops)
                ns[m++] = win_ns(&rc.samples[mode][i]) /
                          (double)rc.samples[mode][i].ops;
        if (!m)
            continue;
        double *sorted = bench_xmalloc(sizeof(double) * (size_t)m);
        memcpy(sorted, ns, sizeof(double) * (size_t)m);
        double med = median_of(sorted, m);
        double lo = sorted[0], hi = sorted[m - 1];
        double cv = cv_of(ns, m);
        free(sorted);

        /* 取 ns 最接近中位数的样本作为计数器/性能代表 */
        int mi = 0;
        double best = 1e30;
        for (int i = 0; i < m; i++) {
            double d = fabs(ns[i] - med);
            if (d < best) {
                best = d;
                mi = i;
            }
        }
        kern_meas *ms = &rc.samples[mode][mi];
        if (mode == 0)
            b0_med = med;

        double ovp = 0.0;
        if (mode > 0 && b0_med > 0)
            ovp = (med - b0_med) / b0_med * 100.0;

        /* §9.3 一致性：慢路径计数 */
        uint64_t exp = (mode == 2) ? pat->expected_creates(&rc, 2) : 0;
        char xnote[192];
        snprintf(xnote, sizeof(xnote), "%s%s",
                 notes,
                 (ms->creates != exp) ? ";CREATE-MISMATCH" : "");
        if (ms->creates != exp)
            fprintf(stderr,
                    "bench: %s th=%d ratio=%.2f lat=%lluus mode=%s creates %llu != expected %llu\n",
                    pat->name, rc.p.nthreads, rc.p.invalid_ratio,
                    (unsigned long long)(rc.p.latency_ns / 1000),
                    mode == 1 ? "B1" : "B2",
                    (unsigned long long)ms->creates, (unsigned long long)exp);

        /* §9.3 形态：T1 0% B1 的 instr_per_op = B0 + VAMAP_CONV_INSNS */
        if (pat->id == 1 && rc.p.invalid_ratio == 0.0 && mode == 1 && b0_n > 0) {
            kern_meas *b0 = &rc.samples[0][0];
            double d0 = b0->ops ? (double)b0->perf[PC_INSTR] / (double)b0->ops : 0;
            double d1 = ms->ops ? (double)ms->perf[PC_INSTR] / (double)ms->ops : 0;
            double delta = d1 - d0;
            if (fabs(delta - (double)VAMAP_CONV_INSNS) > 1.5) {
                fprintf(stderr,
                        "bench: T1 instr_per_op delta=%.2f expected %d (fastpath shape?)\n",
                        delta, VAMAP_CONV_INSNS);
                snprintf(xnote, sizeof(xnote), "%s;INSTR-DEVIATION(%.1f)",
                         notes, delta);
            } else {
                snprintf(xnote, sizeof(xnote), "%s;INSTR-OK(+%.1f)", notes, delta);
            }
        }

        if (csv)
            bench_emit_row(csv, build_tag, pat, &rc, mode, ms, med, lo, hi,
                           ovp, cv, xnote);
        if (mode == 0)
            printf("%-15s th=%-2d r=%.2f lat=%5.0fus : B0 %9.2f ns/op",
                   pat->name, rc.p.nthreads, rc.p.invalid_ratio,
                   (double)rc.p.latency_ns / 1000.0, med);
        else
            printf(" %s %9.2f (%+6.1f%%)",
                   mode == 1 ? "| B1" : "| B2", med, ovp);
        if (mode == 2)
            printf(" | cv=%.1f%%%s\n", cv * 100.0,
                   ms->creates != pat->expected_creates(&rc, 2) ? " [creates!]" : "");
        fflush(stdout);
    }

    pat->fini(&rc);
    pthread_barrier_destroy(&rc.bar);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 单用例 / 矩阵                                                        */
/* ------------------------------------------------------------------ */

static run_params build_params(const opts_t *o, int nth)
{
    run_params p;
    memset(&p, 0, sizeof(p));
    p.invalid_ratio = o->invalid;
    p.latency_ns = o->latency_ns;
    p.latency_mode = o->latency_mode;
    p.alias = o->alias;
    p.scope_rand = o->scope_rand;
    p.ops = o->ops;
    p.reps = o->reps;
    p.max_creates = o->max_creates;
    p.nthreads = nth > MAX_THREADS ? MAX_THREADS : (nth > 0 ? nth : 1);
    p.idx_shared = o->idx_shared;
    p.var_u = o->var_u;
    p.var_u2 = o->var_u2;
    p.variant = o->variant;
    p.npin = g_npin;
    p.pin = g_pin;
    p.suite = g_suite;
    return p;
}

static int run_single(const opts_t *o, const char *build_tag)
{
    const pattern_ops *pat = pattern_by_id(o->test_id);
    if (!pat) {
        fprintf(stderr, "vamap_bench: unknown test T%d\n", o->test_id);
        return 2;
    }
    FILE *csv = stdout;
    if (o->out && o->out[0]) {
        csv = fopen(o->out, "w");
        if (!csv) {
            fprintf(stderr, "vamap_bench: cannot open %s\n", o->out);
            return 2;
        }
    }
    bench_csv_header(csv);
    for (int ti = 0; ti < o->nth; ti++) {
        run_params p = build_params(o, o->th_list[ti]);
        if (bench_run_config(pat, &p, csv, build_tag) != 0)
            return 1;
    }
    if (csv != stdout)
        fclose(csv);
    return 0;
}

/* 矩阵变体（full 档展开；smoke 档 T10 用 W 列表） */
typedef struct {
    int      pat;
    uint64_t u, u2;
    const char *name;
} var_t;

static const var_t g_full_vars[] = {
    { 1,  64,       0, "W=64B"    },
    { 1,  4096,     0, "W=4KiB"   },
    { 1,  2097152,  0, "W=2MiB"   },
    { 7,  1,        0, "S=1"      },
    { 7,  8,        0, "S=8"      },
    { 7,  64,       0, "S=64"     },
    { 9,  500,      0, "s=0.5"    },
    { 9,  900,      0, "s=0.9"    },
    { 9,  1500,     0, "s=1.5"    },
    { 10, 1024,     0, "W=1K"     },
    { 10, 65536,    0, "W=64K"    },
    { 10, 1048576,  0, "W=1M"     },
    { 10, 4194304,  0, "W=4M"     },
    { 12, 256,      0, "K=256"    },
    { 12, 4096,     0, "K=4K"     },
    { 2,  0, 67108864, "DW=64M"   },
};

static int variants_for(int pat, int level, var_t *out, int cap)
{
    int n = 0;
    if (level == 3) {
        for (size_t i = 0; i < sizeof(g_full_vars) / sizeof(g_full_vars[0]); i++)
            if (g_full_vars[i].pat == pat && n < cap)
                out[n++] = g_full_vars[i];
    } else if (level == 1 && pat == 10) {
        if (n < cap) out[n++] = (var_t){ 10, 1,       0, "W=1(4entry)" };
        if (n < cap) out[n++] = (var_t){ 10, 4096,    0, "W=4K"        };
        if (n < cap) out[n++] = (var_t){ 10, 1048576, 0, "W=1M"        };
    }
    if (n == 0)
        out[n++] = (var_t){ pat, 0, 0, "" };
    return n;
}

static const double   MAT_RATIOS[5] = { 0.0, 0.25, 0.5, 0.75, 1.0 };
static const uint64_t MAT_LATS[4]   = { 10000, 100000, 1000000, 10000000 };

static int run_matrix(const opts_t *o, FILE *csv, const char *build_tag)
{
    static const struct { const char *name; } lvlname[] = { {""}, {"smoke"}, {"standard"}, {"full"} };
    printf("== matrix level=%s patterns=%d threads=%d ratios=5 lats=4 ==\n",
           lvlname[o->level].name, pattern_count(), o->nth);
    for (int pi = 0; pi < pattern_count(); pi++) {
        const pattern_ops *pat = pattern_at(pi);
        var_t vars[16];
        int nv = variants_for(pat->id, o->level, vars, 16);
        for (int vi = 0; vi < nv; vi++) {
            for (int ti = 0; ti < o->nth; ti++) {
                for (int ri = 0; ri < 5; ri++) {
                    for (int li = 0; li < 4; li++) {
                        opts_t oo = *o;
                        oo.var_u = vars[vi].u;
                        oo.var_u2 = vars[vi].u2;
                        oo.variant = vars[vi].name;
                        oo.invalid = MAT_RATIOS[ri];
                        oo.latency_ns = MAT_LATS[li];
                        run_params p = build_params(&oo, o->th_list[ti]);
                        if (bench_run_config(pat, &p, csv, build_tag) != 0)
                            return 1;
                    }
                }
            }
        }
        fflush(stdout);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    opts_t o;
    memset(&o, 0, sizeof(o));
    o.reps = 0;                             /* 未指定 → 按 level 默认 */
    o.th_list[0] = 1;
    o.nth = 1;

    /* 读内核时钟源（clocksource=hpet 时内核已判定 rdtsc 跨核不同步，
     * ns 窗口一律取墙钟；notes 记录） */
    {
        FILE *f = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
        if (f) {
            if (fgets(g_clksrc, sizeof(g_clksrc), f)) {
                size_t L = strlen(g_clksrc);
                while (L && (g_clksrc[L - 1] == '\n' || g_clksrc[L - 1] == ' '))
                    g_clksrc[--L] = 0;
                if (!L)
                    snprintf(g_clksrc, sizeof(g_clksrc), "tsc");
            }
            fclose(f);
        }
    }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXTV() (i + 1 < argc ? argv[++i] : (usage(), exit(2), (const char *)""))
        if (!strcmp(a, "--suite") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "correctness"))
                o.suite_correctness = 1;
        } else if (!strcmp(a, "--test") && i + 1 < argc) {
            i++;
            o.test_id = (int)strtol(argv[i] + (argv[i][0] == 'T' || argv[i][0] == 't'), NULL, 10);
        } else if (!strcmp(a, "--matrix")) {
            o.matrix = 1;
        } else if (!strcmp(a, "--level") && i + 1 < argc) {
            i++;
            o.level = !strcmp(argv[i], "smoke") ? 1 :
                      !strcmp(argv[i], "standard") ? 2 :
                      !strcmp(argv[i], "full") ? 3 : 1;
        } else if (!strcmp(a, "--threads") && i + 1 < argc) {
            i++;
            o.nth = parse_int_list(argv[i], o.th_list, 8);
            o.nth_set = 1;
        } else if (!strcmp(a, "--idx-mode") && i + 1 < argc) {
            i++;
            o.idx_shared = !strcmp(argv[i], "shared");
        } else if (!strcmp(a, "--invalid") && i + 1 < argc) {
            i++;
            o.invalid = strtod(argv[i], NULL);
        } else if (!strcmp(a, "--invalid-scope") && i + 1 < argc) {
            i++;
            o.scope_rand = !strcmp(argv[i], "rand");
        } else if (!strcmp(a, "--latency") && i + 1 < argc) {
            i++;
            o.latency_ns = parse_time_ns(argv[i]);
        } else if (!strcmp(a, "--latency-mode") && i + 1 < argc) {
            i++;
            o.latency_mode = !strcmp(argv[i], "sleep") ? VAMAP_LATENCY_SLEEP
                                                       : VAMAP_LATENCY_SPIN;
        } else if (!strcmp(a, "--alias") && i + 1 < argc) {
            i++;
            o.alias = (int)strtol(argv[i], NULL, 10);
        } else if (!strcmp(a, "--ops") && i + 1 < argc) {
            i++;
            o.ops = (uint64_t)strtoull(argv[i], NULL, 0);
        } else if (!strcmp(a, "--reps") && i + 1 < argc) {
            i++;
            o.reps = (int)strtol(argv[i], NULL, 10);
        } else if (!strcmp(a, "--max-creates-per-rep") && i + 1 < argc) {
            i++;
            o.max_creates = (uint64_t)strtoull(argv[i], NULL, 0);
        } else if (!strcmp(a, "--pin") && i + 1 < argc) {
            i++;
            g_npin = bench_parse_cpulist(argv[i], g_pin, 256);
        } else if (!strcmp(a, "--var-u") && i + 1 < argc) {
            i++;
            o.var_u = (uint64_t)strtoull(argv[i], NULL, 0);
        } else if (!strcmp(a, "--var-u2") && i + 1 < argc) {
            i++;
            o.var_u2 = (uint64_t)strtoull(argv[i], NULL, 0);
        } else if (!strcmp(a, "--variant") && i + 1 < argc) {
            i++;
            o.variant = argv[i];
        } else if (!strcmp(a, "--out") && i + 1 < argc) {
            i++;
            o.out = argv[i];
        } else if (!strcmp(a, "--list")) {
            o.list = 1;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "vamap_bench: unknown arg %s\n", a);
            usage();
            return 2;
        }
    }

    static char tag[64];
#ifdef VAMAP_FAST_TAG0
    snprintf(tag, sizeof(tag), "fast-tag0");
#else
    snprintf(tag, sizeof(tag), "strict");
#endif
    if (VAMAP_IDX_BITS != 20) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "-idx%d", VAMAP_IDX_BITS);
        strncat(tag, tmp, sizeof(tag) - strlen(tag) - 1);
    }

    if (o.list) {
        printf("patterns:\n");
        for (int i = 0; i < pattern_count(); i++)
            printf("  T%-2d %s (pool_need=%u)\n", pattern_at(i)->id,
                   pattern_at(i)->name, pattern_at(i)->pool_need);
        return 0;
    }

    if (o.suite_correctness) {
        int fails = bench_run_correctness();
        vamap_shutdown();
        return fails ? 1 : 0;
    }

    if (o.matrix) {
        g_suite = "matrix";
        if (!o.level)
            o.level = 1;
        if (!o.nth_set) {
            static const int th_smoke[]  = { 1, 4 };
            static const int th_std[]    = { 1, 2, 4, 8 };
            static const int th_full[]   = { 1, 2, 4, 8, 16 };
            const int *src = o.level == 1 ? th_smoke :
                             o.level == 2 ? th_std : th_full;
            int n = o.level == 1 ? 2 : o.level == 2 ? 4 : 5;
            memcpy(o.th_list, src, sizeof(int) * (size_t)n);
            o.nth = n;
        }
        if (!o.ops)
            o.ops = o.level == 1 ? (1ULL << 20) : (1ULL << 26);
        if (!o.reps)
            o.reps = o.level == 1 ? 2 : 5;
        if (!o.max_creates)
            o.max_creates = o.level == 1 ? 64 : 4096;

        FILE *csv = stdout;
        char path[512];
        if (o.out && o.out[0]) {
            struct stat st;
            int isdir = (stat(o.out, &st) == 0 && S_ISDIR(st.st_mode)) ||
                        o.out[strlen(o.out) - 1] == '/';
            if (isdir) {
                mkdir(o.out, 0755);         /* 忽略 EEXIST */
                snprintf(path, sizeof(path), "%s%sresults.csv",
                         o.out, o.out[strlen(o.out) - 1] == '/' ? "" : "/");
                csv = fopen(path, "w");
            } else {
                csv = fopen(o.out, "w");
            }
            if (!csv) {
                fprintf(stderr, "vamap_bench: cannot open %s\n", o.out);
                return 2;
            }
        }
        bench_csv_header(csv);
        int rc = run_matrix(&o, csv, tag);
        if (csv != stdout)
            fclose(csv);
        vamap_shutdown();
        return rc;
    }

    if (o.test_id) {
        g_suite = "single";
        if (!o.reps)
            o.reps = 5;
        if (!o.ops)
            o.ops = 1ULL << 26;
        if (!o.max_creates)
            o.max_creates = 4096;
        int rc = run_single(&o, tag);
        vamap_shutdown();
        return rc;
    }

    usage();
    return 2;
}
