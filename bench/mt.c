/* bench/mt.c —— 多线程并行访存基础设施实现(test.md §4.16/§4.17,C15/C16)
 *
 * 线程模型:mt_init 预创建 T 个常驻 worker(绑核、每线程独立 perf 组),
 * mt_round 以 3 个 barrier 驱动一轮:arm → go → done。内核调用全部在
 * worker 内,主线程只做编排与墙钟计时。
 *
 * 绑核策略(§3.3 固定核):从主线程的 sched_getaffinity 集合取 CPU,
 * 线程 0 留在主线程核,其余避开主线程核;核数不足时允许重叠并告警
 * (结果含调度噪声,报告中标注)。
 *
 * 线程安全注意:
 *  - 每线程 perf 组只开 cycles+ins(HARDWARE 类型),不走 HW_CACHE 双编码
 *    探测 —— 其惰性全局初始化(g_cache_rev,harness.c)非线程安全;
 *  - SEG 变体(段缓存)的 vva2va 含全局可变状态,单线程假设,由 main.c
 *    在 MT 用例入口显式拒绝,本层不再防御;
 *  - g_sink 累积在每线程内核尾部各发生一次,竞争只影响 sink 值本身,
 *    不影响计量(防 DCE 语义成立)。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "mt.h"
#include "harness.h"
#include "vva.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

struct mt_ctx {
    int cpu;                      /* 绑核目标;-1 = 不绑 */
    volatile int quit;            /* mt_exit 置位,worker 见 1 后退出 */
    const struct mt_job *job;
    struct pmuset pmu;            /* 每线程独立组(cycles+ins,pid=0=本线程) */
    double cyc, ins;
};

static struct mt_ctx   *g_ctx;
static pthread_t       *g_th;
static pthread_barrier_t g_b_arm, g_b_go, g_b_done;
static int g_T;
static double g_cyc_thr[MT_MAX_THREADS], g_ins_thr[MT_MAX_THREADS];
static int    g_ok_thr[MT_MAX_THREADS];

static void *mt_worker(void *arg) {
    struct mt_ctx *c = arg;

#if defined(VVA_BASE_PINNED)
    /* pinned 构建:表基址寄存器是每线程属性,worker 须各自钉住(design §5.3)。
     * r15/x27 为 ABI callee-saved,libc 函数保留其值,线程内持续驻留安全。 */
    vva_tbl_pin();
#endif

    if (c->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(c->cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
            c->cpu = -1;          /* 绑核失败:继续但不隔离(结果受调度噪声) */
    }
    pmu_open_pair(&c->pmu);       /* pid=0 → 本线程;失败则该线程退化为无 PMU */

    for (;;) {
        pthread_barrier_wait(&g_b_arm);
        if (c->quit) break;
        if (c->pmu.ok) { pmu_reset(&c->pmu); pmu_start(&c->pmu); }
        pthread_barrier_wait(&g_b_go);
        c->job->k(c->job->addr, c->job->n, c->job->p1, c->job->p2);
        if (c->pmu.ok) {
            pmu_stop(&c->pmu);
            pmu_read(&c->pmu);
            c->cyc = (double)c->pmu.val[PMU_CYC];
            c->ins = (double)c->pmu.val[PMU_INS];
        } else {
            c->cyc = c->ins = 0;
        }
        pthread_barrier_wait(&g_b_done);
    }
    if (c->pmu.ok) pmu_close(&c->pmu);
    return NULL;
}

int mt_init(int T) {
    if (T <= 0) T = 1;

    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        int sc = sched_getcpu();
        CPU_SET(sc < 0 ? 0 : sc, &set);
    }
    int navail = CPU_COUNT(&set);
    int self_cpu = sched_getcpu();
    if (self_cpu < 0 || !CPU_ISSET(self_cpu, &set)) self_cpu = 0;
    if (T > navail)
        fprintf(stderr, "警告:线程数 %d > 可用核数 %d,线程将重叠绑核(结果含调度噪声)\n",
                T, navail);

    g_T = T;
    g_ctx = calloc((size_t)T, sizeof(*g_ctx));
    g_th  = calloc((size_t)T, sizeof(*g_th));
    if (!g_ctx || !g_th) { fprintf(stderr, "mt_init:oom\n"); exit(1); }

    pthread_barrier_init(&g_b_arm,  NULL, (unsigned)T + 1);
    pthread_barrier_init(&g_b_go,   NULL, (unsigned)T + 1);
    pthread_barrier_init(&g_b_done, NULL, (unsigned)T + 1);

    /* CPU 分配:线程 0 固定主线程核;其余在"非主线程核"集合内轮转分配
     * (核数不足时复用,天然避免与主线程强挤同核) */
    int cpus[CPU_SETSIZE], n_nonself = 0;
    for (int ci = 0; ci < CPU_SETSIZE; ci++)
        if (CPU_ISSET(ci, &set) && ci != self_cpu) cpus[n_nonself++] = ci;
    for (int t = 0; t < T; t++) {
        if (t == 0)
            g_ctx[t].cpu = self_cpu;
        else if (n_nonself > 0)
            g_ctx[t].cpu = cpus[(t - 1) % n_nonself];
        else
            g_ctx[t].cpu = -1;    /* 仅主线程核可用:不绑 */
        if (pthread_create(&g_th[t], NULL, mt_worker, &g_ctx[t]) != 0) {
            fprintf(stderr, "mt_init:pthread_create 失败(t=%d):%s\n",
                    t, strerror(errno));
            exit(1);              /* barrier 计数已按 T+1 定员,创建失败不可降级 */
        }
    }
    return g_T;
}

void mt_round(const struct mt_job *jobs, int T, struct mt_stats *st) {
    memset(st, 0, sizeof(*st));
    if (!g_ctx || T <= 0 || T > g_T) return;

    for (int t = 0; t < T; t++) {
        g_ctx[t].job = &jobs[t];
        g_ctx[t].cyc = g_ctx[t].ins = 0;
    }

    struct timespec t0, t1;
    pthread_barrier_wait(&g_b_arm);           /* worker:PMU reset+start */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_barrier_wait(&g_b_go);            /* worker:执行内核 */
    pthread_barrier_wait(&g_b_done);          /* worker:PMU stop+read */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    st->wall_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

    double s_cyc = 0, s_ins = 0, cmax = 0, cmin = 0;
    int okn = 0;
    for (int t = 0; t < T; t++) {
        if (g_ctx[t].pmu.ok && g_ctx[t].cyc > 0) {
            s_cyc += g_ctx[t].cyc;
            s_ins += g_ctx[t].ins;
            if (!okn || g_ctx[t].cyc > cmax) cmax = g_ctx[t].cyc;
            if (!okn || g_ctx[t].cyc < cmin) cmin = g_ctx[t].cyc;
            okn++;
        }
    }
    st->cyc_sum = s_cyc; st->ins_sum = s_ins;
    st->cyc_max = cmax;  st->cyc_min = cmin;
    st->cyc_ok_n = okn;
    for (int t = 0; t < T; t++) {
        g_cyc_thr[t] = g_ctx[t].cyc;
        g_ins_thr[t] = g_ctx[t].ins;
        g_ok_thr[t]  = g_ctx[t].pmu.ok;
    }
    st->cyc_thr = g_cyc_thr;
    st->ins_thr = g_ins_thr;
    st->ok_thr  = g_ok_thr;
}

void mt_exit(void) {
    if (!g_ctx) return;
    for (int t = 0; t < g_T; t++) g_ctx[t].quit = 1;
    /* 单次 arm barrier 放行全部 worker(它们同在一处等待;逐线程等待会死锁) */
    pthread_barrier_wait(&g_b_arm);
    for (int t = 0; t < g_T; t++)
        pthread_join(g_th[t], NULL);
    pthread_barrier_destroy(&g_b_arm);
    pthread_barrier_destroy(&g_b_go);
    pthread_barrier_destroy(&g_b_done);
    free(g_ctx); free(g_th);
    g_ctx = NULL; g_th = NULL; g_T = 0;
}
