/*
 * correctness.c — C1–C7 正确性套件（test.md §5）：性能测试的门槛。
 * 每个用例独立 init/shutdown runtime（默认 create_latency_ns = 0；C6 显式
 * 遍历 4 档时延）。返回失败的用例数。
 */
#define _GNU_SOURCE
#include "bench.h"

#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

static int         g_fail;
static const char *g_name;

#define CK(cond)                                                            \
    do {                                                                    \
        if (!(cond)) {                                                      \
            g_fail++;                                                       \
            printf("      FAIL %s:%d: %s\n", g_name, __LINE__, #cond);      \
        }                                                                   \
    } while (0)

static inline uint32_t region_of(uint64_t entry)
{
    return (uint32_t)((entry - (uint64_t)vamap_arena) >> 32);
}

static void cfg_default(vamap_cfg *c, uint32_t pool)
{
    memset(c, 0, sizeof(*c));
    c->lock_stripes = 4096;
    c->alias_pool_size = pool;
    c->create_latency_ns = 0;
}

/* ------------------------------------------------------------------ */
/* C1 转换位精确性                                                      */
/* ------------------------------------------------------------------ */

static int c1_run(void)
{
    vamap_cfg c;
    cfg_default(&c, 64);
    if (vamap_init(&c) != 0)
        return -1;

    /* 模型：idx → region = idx % 64 + 1（alias 预填全部 2^20 表项） */
    uint8_t *model = malloc(VAMAP_NIDX);
    CK(model != NULL);
    if (!model) {
        vamap_shutdown();
        return -1;
    }
    int prefill_fail = 0;
    for (uint32_t i = 0; i < VAMAP_NIDX; i++) {
        uint32_t r = i % 64 + 1;
        model[i] = (uint8_t)r;
        if (vamap_map_alias(i, r) != 0 && !prefill_fail++) {
            printf("      (alias failed at idx=%u)\n", i);
            g_fail++;
        }
    }
    CK(prefill_fail == 0);

    /* 边界组合：idx ∈ {0, 2^20−1} × offset ∈ {0, 2^32−1} × tag ∈ {0, 全1} */
    static const uint32_t BIDX[2] = { 0, VAMAP_IDX_MASK };
    static const uint32_t BOFF[2] = { 0, 0xFFFFFFFFu };
    static const uint64_t BTAG[2] = { 0, 0xFFFULL };
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++)
            for (int t = 0; t < 2; t++) {
                uint64_t vva = (BTAG[t] << 52) | ((uint64_t)BIDX[a] << 32) | BOFF[b];
                void *va = vva2va(vva);
                uint64_t want = (uint64_t)vamap_arena +
                                ((uint64_t)model[BIDX[a]] << 32) + (uint64_t)BOFF[b];
                if ((uint64_t)(uintptr_t)va != want)
                    printf("      (boundary vva=%016llx got=%016llx want=%016llx)\n",
                           (unsigned long long)vva,
                           (unsigned long long)(uintptr_t)va,
                           (unsigned long long)want);
                CK((uint64_t)(uintptr_t)va == want);
            }

    /* 10^7 随机 VVA（随机 tag / 全 32 位 offset）与模型逐位比对 */
    vamap_reset_stats();
    uint64_t s = 0xC1BA5EEDBA11C0DEULL;
    for (long i = 0; i < 10000000; i++) {
        uint64_t tag = xs64(&s) & 0xFFF;
        uint32_t idx = (uint32_t)(xs64(&s) >> 20) & VAMAP_IDX_MASK;
        uint32_t off = (uint32_t)xs64(&s);
#ifdef VAMAP_FAST_TAG0
        tag = 0;                            /* fast 构建约定 tag≡0 */
#endif
        uint64_t vva = (tag << 52) | ((uint64_t)idx << 32) | off;
        void *va = vva2va(vva);
        uint64_t want = (uint64_t)vamap_arena +
                        ((uint64_t)model[idx] << 32) + (uint64_t)off;
        if ((uint64_t)(uintptr_t)va != want) {
            if (g_fail < 5)
                printf("      (rand vva=%016llx got=%016llx want=%016llx)\n",
                       (unsigned long long)vva,
                       (unsigned long long)(uintptr_t)va,
                       (unsigned long long)want);
            g_fail++;
        }
    }
    vamap_stats st = vamap_get_stats();
    CK(st.slow_calls == 0);                 /* 转换不触发慢路径 */
    CK(st.creates == 0);                    /* 不消耗/耗尽 region */

    free(model);
    vamap_shutdown();
    return 0;
}

/* ------------------------------------------------------------------ */
/* C2 create/destroy 往返                                               */
/* ------------------------------------------------------------------ */

static int c2_run(void)
{
    vamap_cfg c;
    cfg_default(&c, 0);
    if (vamap_init(&c) != 0)
        return -1;

    vamap_reset_stats();
    CK(vamap_map_create(100) == 0);
    uint64_t e100 = vamap_table[100];
    CK(e100 != 0);
    CK(vamap_map_create(100) == 0);         /* 幂等 */
    CK(vamap_table[100] == e100);
    vamap_stats st = vamap_get_stats();
    CK(st.creates == 1);                    /* 未重复创建 */

    CK(vamap_map_create(200) == 0);
    uint64_t e200 = vamap_table[200];
    uint32_t r100 = region_of(e100), r200 = region_of(e200);
    CK(r100 > 0 && r200 > 0 && r100 != r200);

    CK(vamap_map_create(300) == 0);
    uint32_t r300 = region_of(vamap_table[300]);
    CK(r300 != r100 && r300 != r200);       /* 销毁前不复用 */

    CK(vamap_map_destroy(100) == 0);
    CK(vamap_table[100] == 0);
    CK(vamap_map_create(400) == 0);
    CK(region_of(vamap_table[400]) == r100);/* 销毁后才复用 region */

    st = vamap_get_stats();
    CK(st.destroys == 1);
    CK(vamap_map_destroy(500) == 0);        /* 销毁未映射：成功、不计数 */
    st = vamap_get_stats();
    CK(st.destroys == 1);

    vamap_shutdown();
    return 0;
}

/* ------------------------------------------------------------------ */
/* C3 同索引竞争创建                                                    */
/* ------------------------------------------------------------------ */

#define C3_P 8
static uint64_t            c3_vva;
static void               *c3_ptrs[C3_P];
static pthread_barrier_t   c3_bar;

static void *c3_thread(void *arg)
{
    long tid = (long)arg;
    pthread_barrier_wait(&c3_bar);
    void *p = vva2va(c3_vva);
    c3_ptrs[tid] = p;
    if (p)
        *(volatile uint64_t *)p = 0xC0DE0000ULL + (uint64_t)tid;
    return NULL;
}

static int c3_run(void)
{
    vamap_cfg c;
    cfg_default(&c, 0);
    if (vamap_init(&c) != 0)
        return -1;

    c3_vva = (uint64_t)777 << 32;           /* 初始无效 */
    pthread_barrier_init(&c3_bar, NULL, C3_P);
    vamap_reset_stats();

    pthread_t th[C3_P];
    for (long i = 0; i < C3_P; i++)
        pthread_create(&th[i], NULL, c3_thread, (void *)i);
    for (int i = 0; i < C3_P; i++)
        pthread_join(th[i], NULL);
    pthread_barrier_destroy(&c3_bar);

    for (int i = 0; i < C3_P; i++)
        CK(c3_ptrs[i] != NULL && c3_ptrs[i] == c3_ptrs[0]);   /* VA 一致 */
    vamap_stats st = vamap_get_stats();
    CK(st.creates == 1);                    /* 仅 1 次 create */
    CK(st.slow_calls == C3_P);              /* 其余经 double-check 命中 */
    CK(vamap_table[777] != 0);
    uint64_t v = *(volatile uint64_t *)c3_ptrs[0];
    CK(v >= 0xC0DE0000ULL && v < 0xC0DE0000ULL + C3_P);       /* 数据一致 */

    vamap_shutdown();
    return 0;
}

/* ------------------------------------------------------------------ */
/* C4 异索引并发 + 跨线程可见性                                         */
/* ------------------------------------------------------------------ */

static volatile unsigned c4_ready, c4_ack;
static uint64_t          c4_vva;

static void *c4_writer(void *arg)
{
    (void)arg;
    for (int round = 0; round < 100; round++) {
        uint64_t payload = 0xA110C8ED0000ULL + (uint64_t)round;
        __atomic_store_n(&c4_ready, 0, __ATOMIC_RELAXED);
        if (vamap_map_create(888) != 0)
            return NULL;
        c4_vva = (uint64_t)888 << 32;
        *(volatile uint64_t *)(uintptr_t)vva2va(c4_vva) = payload;
        __atomic_store_n(&c4_ready, (unsigned)(round + 1), __ATOMIC_RELEASE);
        while (__atomic_load_n(&c4_ack, __ATOMIC_ACQUIRE) < (unsigned)(round + 1))
            sched_yield();
        vamap_map_destroy(888);
    }
    return NULL;
}

static void *c4_reader(void *arg)
{
    (void)arg;
    for (int round = 0; round < 100;) {
        unsigned r = __atomic_load_n(&c4_ready, __ATOMIC_ACQUIRE);
        if (r != (unsigned)(round + 1)) {
            sched_yield();
            continue;
        }
        uint64_t got = vload64(c4_vva);     /* acquire 链后经快路径读 */
        if (got != 0xA110C8ED0000ULL + (uint64_t)round) {
            g_fail++;
            printf("      (round %d: payload %016llx)\n", round,
                   (unsigned long long)got);
        }
        uint64_t via_ptr = *(volatile uint64_t *)(uintptr_t)vva2va(c4_vva);
        CK(via_ptr == got);
        __atomic_store_n(&c4_ack, (unsigned)(round + 1), __ATOMIC_RELEASE);
        round++;
    }
    return NULL;
}

static int c4_run(void)
{
    vamap_cfg c;
    cfg_default(&c, 0);
    if (vamap_init(&c) != 0)
        return -1;
    c4_ready = c4_ack = 0;
    pthread_t tw, tr;
    pthread_create(&tw, NULL, c4_writer, NULL);
    pthread_create(&tr, NULL, c4_reader, NULL);
    pthread_join(tw, NULL);
    pthread_join(tr, NULL);
    vamap_shutdown();
    return 0;
}

/* ------------------------------------------------------------------ */
/* C5 churn 压力 + 并发读者                                             */
/* ------------------------------------------------------------------ */

#define C5_THREADS 8
#define C5_SPACE   32768
#define C5_PER     125000               /* 8 × 125000 = 10^6 往返 */
static volatile int c5_done;

static void *c5_writer(void *arg)
{
    long tid = (long)arg + 1;               /* 签名 tid ∈ [1, 8] */
    uint32_t per = C5_SPACE / C5_THREADS;
    uint32_t base = (uint32_t)(tid - 1) * per;
    uint64_t s = 0xC5FEED0000ULL + (uint64_t)tid;
    for (uint32_t k = 0; k < C5_PER; k++) {
        uint32_t idx = base + (uint32_t)(xs64(&s) & (per - 1));
        if (vamap_map_create(idx) != 0) {
            __atomic_add_fetch(&g_fail, 1, __ATOMIC_RELAXED);
            return NULL;
        }
        uint64_t *p = (uint64_t *)(uintptr_t)vva2va((uint64_t)idx << 32);
        uint64_t sig = ((uint64_t)tid << 56) | (k & 0x00FFFFFFFFFFFFFFULL);
        *p = sig;
        if (*p != sig)
            __atomic_add_fetch(&g_fail, 1, __ATOMIC_RELAXED);
        if (vamap_map_destroy(idx) != 0)
            __atomic_add_fetch(&g_fail, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void *c5_reader(void *arg)
{
    (void)arg;
    uint64_t s = 0xC5BAD5EEDULL;
    while (!__atomic_load_n(&c5_done, __ATOMIC_ACQUIRE)) {
        uint32_t idx = (uint32_t)(xs64(&s) & (C5_SPACE - 1));
        /* 只读已发布表项（不触发慢路径创建）；region 内存进程内常驻 */
        uint64_t e = __atomic_load_n(&vamap_table[idx], __ATOMIC_ACQUIRE);
        if (e) {
            uint64_t v = *(volatile uint64_t *)(uintptr_t)(e | 0);
            uint64_t tidf = v >> 56;
            if (tidf > C5_THREADS)
                __atomic_add_fetch(&g_fail, 1, __ATOMIC_RELAXED);
        }
    }
    return NULL;
}

static int c5_run(void)
{
    vamap_cfg c;
    cfg_default(&c, 0);
    if (vamap_init(&c) != 0)
        return -1;
    vamap_reset_stats();
    c5_done = 0;
    pthread_t th[C5_THREADS], rd[2];
    for (long i = 0; i < 2; i++)
        pthread_create(&rd[i], NULL, c5_reader, NULL);
    for (long i = 0; i < C5_THREADS; i++)
        pthread_create(&th[i], NULL, c5_writer, (void *)i);
    for (int i = 0; i < C5_THREADS; i++)
        pthread_join(th[i], NULL);
    __atomic_store_n(&c5_done, 1, __ATOMIC_RELEASE);
    pthread_join(rd[0], NULL);
    pthread_join(rd[1], NULL);

    vamap_stats st = vamap_get_stats();
    CK(st.creates == (uint64_t)C5_THREADS * C5_PER);
    CK(st.destroys == (uint64_t)C5_THREADS * C5_PER);
    for (uint32_t i = 0; i < C5_SPACE; i++)
        if (vamap_table[i] != 0) {
            CK(0);                          /* 全部已销毁 */
            break;
        }
    vamap_shutdown();
    return 0;
}

/* ------------------------------------------------------------------ */
/* C6 时延精度（SPIN）                                                  */
/* ------------------------------------------------------------------ */

static int c6_run(void)
{
    vamap_cfg c;
    cfg_default(&c, 0);
    c.latency_mode = VAMAP_LATENCY_SPIN;
    if (vamap_init(&c) != 0)
        return -1;

    static const uint64_t lats[4] = { 10000, 100000, 1000000, 10000000 };
    for (int li = 0; li < 4; li++) {
        for (uint32_t i = 0; i < 64; i++)
            vamap_map_destroy(i);
        vamap_set_create_latency_ns(lats[li]);
        vamap_reset_stats();
        for (uint32_t i = 0; i < 64; i++)
            CK(vamap_map_create(i) == 0);
        vamap_stats st = vamap_get_stats();
        CK(st.creates == 64);
        double per = (double)st.spin_ns / 64.0;
        double dev = (per - (double)lats[li]) / (double)lats[li] * 100.0;
        printf("      lat=%6.0fus measured=%.0fns/creates dev=%+.2f%%\n",
               (double)lats[li] / 1000.0, per, dev);
        CK(fabs(dev) <= 5.0);
        vamap_set_create_latency_ns(0);
        for (uint32_t i = 0; i < 64; i++)
            vamap_map_destroy(i);
    }
    vamap_shutdown();
    return 0;
}

/* ------------------------------------------------------------------ */
/* C7 alias 共享与销毁                                                  */
/* ------------------------------------------------------------------ */

static int c7_run(void)
{
    /* 非 alias 路径：引用计数 */
    vamap_cfg c;
    cfg_default(&c, 0);
    if (vamap_init(&c) != 0)
        return -1;

    CK(vamap_map_create(10) == 0);
    uint64_t e10 = vamap_table[10];
    uint32_t R = region_of(e10);
    CK(R > 0);
    CK(vamap_map_alias(20, R) == 0);        /* 两 idx alias 同一 region */
    CK(vamap_table[20] == e10);

    CK(vamap_map_destroy(10) == 0);
    CK(vamap_table[10] == 0);
    CK(vamap_table[20] != 0);               /* 另一个仍有效 */
    CK(vload64((uint64_t)20 << 32) == 0);   /* 有效访问（region 未提前回收） */

    CK(vamap_map_destroy(20) == 0);
    CK(vamap_map_create(30) == 0);
    CK(region_of(vamap_table[30]) == R);    /* 最后一个销毁后才复用 */

    /* 负例 */
    CK(vamap_map_alias(40, 0) != 0);
    CK(vamap_map_alias(40, 0xF0000000u) != 0);
    CK(vamap_map_create(50) == 0);
    CK(vamap_map_alias(50, region_of(vamap_table[50])) != 0);   /* 已有效拒绝 */
    CK(vamap_map_destroy(50) == 0);

    /* 引用计数 2：销毁一个仍有效 */
    CK(vamap_map_alias(60, region_of(vamap_table[30])) == 0);
    CK(vamap_table[60] == vamap_table[30]);
    vamap_map_destroy(30);
    CK(vamap_table[60] != 0);
    vamap_map_destroy(60);
    vamap_shutdown();

    /* 池 region alias：无需预先 create */
    cfg_default(&c, 4);
    if (vamap_init(&c) != 0)
        return -1;
    CK(vamap_map_alias(100, 3) == 0);
    CK(region_of(vamap_table[100]) == 3);
    CK(vamap_map_destroy(100) == 0);
    CK(vamap_map_create(101) == 0);
    uint32_t r101 = region_of(vamap_table[101]);
    CK(r101 >= 1 && r101 <= 4);             /* 池内复用 */
    vamap_shutdown();
    return 0;
}

/* ------------------------------------------------------------------ */
/* 套件入口                                                             */
/* ------------------------------------------------------------------ */

int bench_run_correctness(void)
{
    static const struct {
        const char *id;
        const char *name;
        int (*fn)(void);
    } T[] = {
        { "C1", "转换位精确性（alias 池预填 2^20 + 10^7 随机 VVA）", c1_run },
        { "C2", "create/destroy 往返与 region 复用",                 c2_run },
        { "C3", "同索引竞争创建（8 线程）",                          c3_run },
        { "C4", "异索引并发 + release/acquire 可见性",               c4_run },
        { "C5", "churn 压力（8 线程 10^6 往返 + 并发读者）",         c5_run },
        { "C6", "时延精度（SPIN，4 档 ≤ ±5%）",                      c6_run },
        { "C7", "alias 共享与引用计数销毁",                          c7_run },
    };
    int fails = 0;
    printf("== correctness suite (create_latency_ns=0, C6 除外) ==\n");
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        g_fail = 0;
        g_name = T[i].id;
        int rc = T[i].fn();
        int ok = (rc == 0 && g_fail == 0);
        printf("[%s] %-38s %s%s\n", T[i].id, T[i].name, ok ? "PASS" : "FAIL",
               rc != 0 ? " (setup/init error)" : "");
        fflush(stdout);
        if (!ok)
            fails++;
    }
    printf("correctness: %zu/%zu passed\n",
           sizeof(T) / sizeof(T[0]) - (size_t)fails, sizeof(T) / sizeof(T[0]));
    return fails;
}
