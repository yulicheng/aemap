/*
 * vamap_runtime.c — VVA→VA 动态地址映射 mock 运行时库（design.md §4/§6）。
 *
 *  - 映射表：init 时匿名 mmap 2^VAMAP_IDX_BITS × 8B（默认 8 MiB），
 *    2 MiB 对齐并建议大页驻留（MAP_HUGETLB → madvise(MADV_HUGEPAGE) 回退）。
 *  - 竞技场：REGION_COUNT × 4 GiB 虚拟预留（MAP_NORESERVE），4 GiB 对齐；
 *    REGION_COUNT 按平台用户 VA 上限自高向低按 2 的幂回退。
 *  - region 分配器：全局自旋锁 + 空闲链 + 每 region 引用计数；
 *    alias 池模式下 [1, pool] 区间按轮转/引用计数复用，永不归还普通分配器。
 *  - 慢路径：条带锁 + double-check + 时延模拟（SPIN: rdtsc 忙等；
 *    SLEEP: 可睡眠互斥锁 + clock_nanosleep）+ release 原子发布。
 *  - 锁顺序统一为 条带锁 → 全局自旋锁。
 */
#define _GNU_SOURCE
#include "vamap.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif
#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */

uint64_t *vamap_table = NULL;
uint64_t  vamap_arena = 0;
static uint32_t g_region_count;      /* 竞技场 region 总数（含保留 0 号） */
static uint32_t g_region_limit;      /* 可分配 region 号上界 */
static uint32_t g_pool_size;         /* alias 池大小（0=禁用） */
static uint32_t g_pool_rr;           /* 池轮转游标 */
static int      g_init = 0;
static vamap_cfg g_cfg;

static uint32_t *g_refcnt;           /* 每 region 引用计数 */
static uint32_t *g_free_stack;       /* 空闲 region 号栈（LIFO） */
static uint32_t  g_free_top;

static size_t    g_table_bytes;
static size_t    g_arena_bytes;
static char      g_backing[32];      /* 映射表驻留方式记录 */

/* 慢路径计数（原子增量；快路径不写任何计数，见 design.md §6.1） */
static uint64_t s_creates, s_destroys, s_spin_ns, s_slow_ns_total, s_slow_calls;

/* ------------------------------------------------------------------ */
/* 自旋锁与 CPU pause                                                  */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t f; } vspin_t;

static inline void cpu_relax(void)
{
#if defined(__x86_64__)
    __asm__ volatile ("pause");
#elif defined(__aarch64__)
    __asm__ volatile ("yield");
#else
    do { } while (0);
#endif
}

static inline void vspin_lock(vspin_t *l)
{
    uint32_t e = 0;
    for (;;) {
        if (__atomic_compare_exchange_n(&l->f, &e, 1u, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        while (__atomic_load_n(&l->f, __ATOMIC_RELAXED))
            cpu_relax();
        e = 0;
    }
}

static inline void vspin_unlock(vspin_t *l)
{
    __atomic_store_n(&l->f, 0u, __ATOMIC_RELEASE);
}

/* 条带锁：SPIN 模式用自旋锁，SLEEP 模式用可睡眠互斥锁
 * （不得持自旋锁进入 clock_nanosleep，design.md §6.3）。
 * 64B 对齐 + padding，避免伪共享（design.md §7）。 */
typedef struct {
    union {
        vspin_t         spin;
        pthread_mutex_t mtx;
    } u;
    char _pad[128];
} stripe_t __attribute__((aligned(64)));

static stripe_t *g_stripes;

static inline void stripe_lock(stripe_t *s)
{
    if (g_cfg.latency_mode == VAMAP_LATENCY_SLEEP)
        pthread_mutex_lock(&s->u.mtx);
    else
        vspin_lock(&s->u.spin);
}
static inline void stripe_unlock(stripe_t *s)
{
    if (g_cfg.latency_mode == VAMAP_LATENCY_SLEEP)
        pthread_mutex_unlock(&s->u.mtx);
    else
        vspin_unlock(&s->u.spin);
}

/* 全局 region 分配器自旋锁（临界区极短，不睡眠；两种模式均可用） */
static vspin_t g_region_lk;

/* ------------------------------------------------------------------ */
/* TSC 计时与时延模拟                                                  */
/* ------------------------------------------------------------------ */

static double g_tsc_hz = 1e9;        /* TSC 频率（cycles per second） */
static double g_cycles_per_ns = 1.0;

uint64_t vamap_wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#if defined(__x86_64__)
static inline uint64_t tsc_raw(void)
{
    uint32_t a, d;
    __asm__ volatile ("rdtsc" : "=a"(a), "=d"(d));
    return (uint64_t)a | ((uint64_t)d << 32);
}
#elif defined(__aarch64__)
static inline uint64_t tsc_raw(void)
{
    uint64_t v;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
#else
/* 无硬件计数器平台：以单调墙钟作为“TSC”（g_tsc_hz = 1e9） */
static inline uint64_t tsc_raw(void) { return vamap_wall_ns(); }
#endif

static inline uint64_t ns_to_cycles(uint64_t ns)
{
    return (uint64_t)((double)ns * g_cycles_per_ns);
}
static inline uint64_t cycles_to_ns(uint64_t c)
{
    return (uint64_t)((double)c * 1e9 / g_tsc_hz);
}

static inline void spin_until(uint64_t deadline)
{
    while (tsc_raw() < deadline)
        cpu_relax();
}

static void sleep_ns(uint64_t ns)
{
    struct timespec ts, dl;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    dl.tv_sec  = ts.tv_sec  + (time_t)(ns / 1000000000ULL);
    dl.tv_nsec = ts.tv_nsec + (long)(ns % 1000000000ULL);
    if (dl.tv_nsec >= 1000000000L) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, NULL) == EINTR)
        ;
}

/* 用 CLOCK_MONOTONIC 标定 TSC 频率（两次采样间隔约 20ms） */
static void calibrate_tsc(void)
{
#if defined(__aarch64__)
    uint64_t frq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(frq));
    if (frq) {
        g_tsc_hz = (double)frq;
        g_cycles_per_ns = g_tsc_hz / 1e9;
        return;
    }
#endif
    uint64_t w0 = vamap_wall_ns(), c0 = tsc_raw(), w1, c1;
    for (;;) {
        struct timespec ts = { 0, 5000000L }; /* 5ms */
        nanosleep(&ts, NULL);
        w1 = vamap_wall_ns();
        if (w1 - w0 >= 20000000ULL)
            break;
    }
    c1 = tsc_raw();
    if (w1 > w0 && c1 > c0) {
        g_tsc_hz = (double)(c1 - c0) * 1e9 / (double)(w1 - w0);
        if (g_tsc_hz < 1e6)
            g_tsc_hz = 1e6;
    }
    g_cycles_per_ns = g_tsc_hz / 1e9;
}

double vamap_tsc_hz(void) { return g_tsc_hz; }

/* ------------------------------------------------------------------ */
/* region 分配器（design.md §6.5）：全局自旋锁 + 空闲链 + 引用计数      */
/* 调用方持有条带锁；本函数内只取全局锁，锁顺序 条带锁 → 全局锁          */
/* ------------------------------------------------------------------ */

static inline int is_pool_region(uint32_t r)
{
    return g_pool_size > 0 && r >= 1 && r <= g_pool_size;
}

/* 返回 0 表示 region 号耗尽 */
static uint32_t region_alloc(void)
{
    uint32_t r = 0;
    vspin_lock(&g_region_lk);
    if (g_pool_size > 0) {
        /* alias 池模式：从池中按轮转取用（引用计数 +1），不消耗池外新 region */
        r = 1 + (g_pool_rr++ % g_pool_size);
        g_refcnt[r]++;
    } else if (g_free_top > 0) {
        r = g_free_stack[--g_free_top];
        g_refcnt[r] = 1;
    }
    vspin_unlock(&g_region_lk);
    return r;
}

/* 引用计数归零时回收：池内 region 留在池中供复用（不归还空闲链），
 * 普通 region 归还空闲链（design.md §6.4） */
static void region_release(uint32_t r)
{
    vspin_lock(&g_region_lk);
    if (g_refcnt[r] > 0 && --g_refcnt[r] == 0) {
        if (!is_pool_region(r) && g_free_top < g_region_count) {
            g_free_stack[g_free_top++] = r;
        }
    }
    vspin_unlock(&g_region_lk);
}

/* ------------------------------------------------------------------ */
/* 慢路径核心：条带锁 + double-check + 时延模拟 + release 发布          */
/* （design.md §6.3）                                                  */
/* ------------------------------------------------------------------ */

static void *slow_create_core(uint64_t vva)
{
    uint32_t idx = (uint32_t)(vva >> 32) & VAMAP_IDX_MASK;
    stripe_t *lk = &g_stripes[idx & (g_cfg.lock_stripes - 1)];
    int sleep_mode = (g_cfg.latency_mode == VAMAP_LATENCY_SLEEP);
    uint64_t tsc0 = 0, wns0 = 0;
    void *ret;

    __atomic_add_fetch(&s_slow_calls, 1, __ATOMIC_RELAXED);   /* 仅慢路径计数 */

    if (sleep_mode) wns0 = vamap_wall_ns();
    else            tsc0 = tsc_raw();

    stripe_lock(lk);
    {
        uint64_t e = __atomic_load_n(&vamap_table[idx], __ATOMIC_ACQUIRE);
        if (e == 0) {                                         /* double-check：持锁复查 */
            uint32_t region = region_alloc();                 /* 全局自旋锁取区域号（快） */
            if (region == 0) {
                /* region 号耗尽：不发布表项（design.md §6.3 要点 5） */
            } else {
                uint64_t lat = g_cfg.create_latency_ns;
                if (sleep_mode) {
                    if (lat)
                        sleep_ns(lat);                        /* SLEEP：clock_nanosleep 让核 */
                    __atomic_add_fetch(&s_spin_ns, lat, __ATOMIC_RELAXED);
                } else if (lat) {
                    uint64_t c0 = tsc_raw();                  /* SPIN：rdtsc + pause 忙等 */
                    spin_until(c0 + ns_to_cycles(lat));
                    __atomic_add_fetch(&s_spin_ns,
                                       cycles_to_ns(tsc_raw() - c0),
                                       __ATOMIC_RELAXED);
                }
                e = (uint64_t)vamap_arena + ((uint64_t)region << 32);
                __atomic_store_n(&vamap_table[idx], e, __ATOMIC_RELEASE); /* 原子发布 */
                __atomic_add_fetch(&s_creates, 1, __ATOMIC_RELAXED);
            }
        }
        ret = e ? (void *)(e | (uint32_t)vva) : NULL;
    }
    stripe_unlock(lk);

    if (sleep_mode)
        __atomic_add_fetch(&s_slow_ns_total, vamap_wall_ns() - wns0, __ATOMIC_RELAXED);
    else
        __atomic_add_fetch(&s_slow_ns_total, cycles_to_ns(tsc_raw() - tsc0),
                           __ATOMIC_RELAXED);
    return ret;
}

void *vamap_slow_create_fill(uint64_t vva)
{
    if (!g_init)
        return NULL;
    return slow_create_core(vva);
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                            */
/* ------------------------------------------------------------------ */

int vamap_map_create(uint32_t idx)
{
    if (!g_init || idx >= VAMAP_NIDX)
        return -1;
    uint64_t e = __atomic_load_n(&vamap_table[idx], __ATOMIC_ACQUIRE);
    if (e)
        return 0;                          /* 已存在：成功，不计创建/时延 */
    void *va = slow_create_core((uint64_t)idx << 32);
    return va ? 0 : -2;                    /* -2: region 号耗尽等失败 */
}

int vamap_map_destroy(uint32_t idx)
{
    if (!g_init || idx >= VAMAP_NIDX)
        return -1;
    stripe_t *lk = &g_stripes[idx & (g_cfg.lock_stripes - 1)];
    stripe_lock(lk);
    {
        uint64_t e = __atomic_load_n(&vamap_table[idx], __ATOMIC_ACQUIRE);
        __atomic_store_n(&vamap_table[idx], 0, __ATOMIC_RELEASE);
        if (e) {
            uint64_t d = e - (uint64_t)vamap_arena;
            uint32_t r = (uint32_t)(d >> 32);
            if ((d & 0xFFFFFFFFULL) == 0 && r > 0 && r < g_region_count)
                region_release(r);         /* 引用计数归零才真正回收 */
            __atomic_add_fetch(&s_destroys, 1, __ATOMIC_RELAXED);
        }
    }
    stripe_unlock(lk);
    return 0;
}

int vamap_map_alias(uint32_t idx, uint32_t region)
{
    if (!g_init || idx >= VAMAP_NIDX)
        return -1;
    if (region == 0 || region >= g_region_count)
        return -2;
    stripe_t *lk = &g_stripes[idx & (g_cfg.lock_stripes - 1)];
    stripe_lock(lk);
    int rc;
    {
        uint64_t e = __atomic_load_n(&vamap_table[idx], __ATOMIC_ACQUIRE);
        if (e) {
            rc = -3;                       /* 已有效：拒绝，避免覆盖泄漏引用 */
        } else {
            vspin_lock(&g_region_lk);
            /* 仅允许指向池内 region（常驻）或当前被引用的 region */
            int ok = is_pool_region(region) || g_refcnt[region] > 0;
            if (ok)
                g_refcnt[region]++;
            vspin_unlock(&g_region_lk);
            if (ok) {
                __atomic_store_n(&vamap_table[idx],
                                 (uint64_t)vamap_arena + ((uint64_t)region << 32),
                                 __ATOMIC_RELEASE);
                rc = 0;
            } else {
                rc = -4;                   /* region 不可用 */
            }
        }
    }
    stripe_unlock(lk);
    return rc;
}

void vamap_set_create_latency_ns(uint64_t ns)
{
    g_cfg.create_latency_ns = ns;
}

vamap_stats vamap_get_stats(void)
{
    vamap_stats st;
    st.creates       = __atomic_load_n(&s_creates, __ATOMIC_RELAXED);
    st.destroys      = __atomic_load_n(&s_destroys, __ATOMIC_RELAXED);
    st.spin_ns       = __atomic_load_n(&s_spin_ns, __ATOMIC_RELAXED);
    st.slow_ns_total = __atomic_load_n(&s_slow_ns_total, __ATOMIC_RELAXED);
    st.slow_calls    = __atomic_load_n(&s_slow_calls, __ATOMIC_RELAXED);
    return st;
}

void vamap_reset_stats(void)
{
    __atomic_store_n(&s_creates, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_destroys, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_spin_ns, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_slow_ns_total, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_slow_calls, 0, __ATOMIC_RELAXED);
}

void *vamap_pin(uint64_t vva)
{
    if (!g_init)
        return NULL;
    uint32_t idx = (uint32_t)(vva >> 32) & VAMAP_IDX_MASK;
    uint64_t e = __atomic_load_n(&vamap_table[idx], __ATOMIC_ACQUIRE);
    if (e == 0) {
        void *va = vamap_slow_create_fill(vva);
        if (!va)
            return NULL;
        e = (uint64_t)(uintptr_t)va & ~0xFFFFFFFFULL;
    }
    return (void *)(uintptr_t)e;           /* 当前 4GiB 窗口真实基址 */
}

/* ------------------------------------------------------------------ */
/* VVA 索引区间分配（bump + 简单空闲链）                                */
/* ------------------------------------------------------------------ */

static pthread_mutex_t g_vva_lk = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_vva_bump;
#define VVA_FREE_CAP 512
static struct { uint32_t start, n; } g_vva_free[VVA_FREE_CAP];
static int g_vva_nfree;

vva_t vamap_vva_alloc(uint32_t nidx)
{
    if (!g_init || nidx == 0 || nidx > VAMAP_NIDX)
        return (vva_t)-1;
    pthread_mutex_lock(&g_vva_lk);
    vva_t ret = (vva_t)-1;
    for (int i = 0; i < g_vva_nfree; i++) {           /* first-fit 空闲链 */
        if (g_vva_free[i].n >= nidx) {
            uint32_t s = g_vva_free[i].start;
            if (g_vva_free[i].n == nidx) {
                g_vva_free[i] = g_vva_free[--g_vva_nfree];
            } else {
                g_vva_free[i].start += nidx;
                g_vva_free[i].n     -= nidx;
            }
            ret = (vva_t)s << 32;
            goto out;
        }
    }
    if (g_vva_bump + nidx <= VAMAP_NIDX) {
        ret = g_vva_bump << 32;
        g_vva_bump += nidx;
    }
out:
    pthread_mutex_unlock(&g_vva_lk);
    return ret;
}

void vamap_vva_free(vva_t base, uint32_t nidx)
{
    if (!g_init || nidx == 0)
        return;
    pthread_mutex_lock(&g_vva_lk);
    if (g_vva_nfree < VVA_FREE_CAP) {
        g_vva_free[g_vva_nfree].start = (uint32_t)(base >> 32);
        g_vva_free[g_vva_nfree].n     = nidx;
        g_vva_nfree++;
    }                                   /* 空闲链满：放弃回收（bench 场景不适用） */
    pthread_mutex_unlock(&g_vva_lk);
}

/* ------------------------------------------------------------------ */
/* init / shutdown                                                     */
/* ------------------------------------------------------------------ */

static int is_pow2(uint32_t v) { return v && !(v & (v - 1)); }

/* 分配 2 MiB 对齐映射表，优先 MAP_HUGETLB，失败回退 madvise(MADV_HUGEPAGE) */
static int alloc_table(char *backing, size_t bksz)
{
    size_t bytes = (size_t)VAMAP_NIDX * sizeof(uint64_t);
#if defined(MAP_HUGETLB)
    {
        /* 不带 MAP_NORESERVE：mmap 时即预留大页；系统无 hugetlb 池时
         * 直接失败回退（带 NORESERVE 会在首次缺页时 SIGBUS） */
        void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS |
                       MAP_HUGETLB | (21UL << MAP_HUGE_SHIFT), -1, 0);
        if (p != MAP_FAILED) {
            /* 预留成功即可安全触页（大页已在 mmap 时记账） */
            *(volatile uint8_t *)p = 0;
            vamap_table = (uint64_t *)p;
            g_table_bytes = bytes;
            snprintf(backing, bksz, "hugetlb");
            return 0;
        }
    }
#endif
    size_t total = bytes + (2UL << 20);
    void *raw = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED)
        return -1;
    uintptr_t a = ((uintptr_t)raw + (2UL << 20) - 1) & ~((uintptr_t)(2UL << 20) - 1);
    if (a != (uintptr_t)raw)
        munmap(raw, (size_t)(a - (uintptr_t)raw));
    uintptr_t end = a + bytes, rend = (uintptr_t)raw + total;
    if (rend > end)
        munmap((void *)end, (size_t)(rend - end));
    vamap_table = (uint64_t *)a;
    g_table_bytes = bytes;
    if (madvise(vamap_table, bytes, MADV_HUGEPAGE) == 0)
        snprintf(backing, bksz, "thp");
    else
        snprintf(backing, bksz, "default");
    return 0;
}

/* 预留 RC × 4GiB 竞技场，4GiB 对齐裁剪；strict 模式要求整体 ≤ 2^48 */
static uint64_t mmap_arena(uint64_t region_count, int strict)
{
    uint64_t span  = region_count << 32;
    uint64_t total = span + (8ULL << 30);          /* 含对齐余量 */
    const int ntry = strict ? 12 : 1;
    for (int i = 0; i < ntry; i++) {
        uint64_t hint = 0;
        if (strict) {
            /* 优先向内核请求 2^48 以下的地址（design.md §6.2） */
            hint = ((1ULL << 48) - total - ((uint64_t)i << 40)) & ~((1ULL << 32) - 1);
            if (hint < (4ULL << 30))
                hint = 4ULL << 30;
        }
        void *p = mmap(strict ? (void *)(uintptr_t)hint : NULL, total,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
            continue;
        uintptr_t raw = (uintptr_t)p;
        uintptr_t a = (raw + (4ULL << 30) - 1) & ~((uintptr_t)(4ULL << 30) - 1);
        uintptr_t aend = a + span;
        if (strict && aend > (1ULL << 48)) {
            munmap(p, total);
            continue;
        }
        if (a != raw)
            munmap(p, (size_t)(a - raw));
        if ((uintptr_t)raw + total > aend)
            munmap((void *)aend, (size_t)((uintptr_t)raw + total - aend));
        return (uint64_t)a;
    }
    return 0;
}

int vamap_init(const vamap_cfg *cfg)
{
    if (g_init)
        vamap_shutdown();

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.create_latency_ns = 0;
    g_cfg.latency_mode      = VAMAP_LATENCY_SPIN;
    g_cfg.lock_stripes      = 4096;
    g_cfg.alias_pool_size   = 0;
    g_cfg.strict_48bit      = 0;
    if (cfg)
        g_cfg = *cfg;
    if (!is_pow2(g_cfg.lock_stripes))
        return -1;
    if (g_cfg.latency_mode != VAMAP_LATENCY_SPIN &&
        g_cfg.latency_mode != VAMAP_LATENCY_SLEEP)
        return -1;

    char backing[32] = "default";
    if (alloc_table(backing, sizeof(backing)) != 0)
        return -1;

    /* REGION_COUNT 按平台用户 VA 上限自高向低按 2 的幂回退（design.md §6.2），
     * 取首个可成功预留并为进程自身映射留出余量的档位 */
    static const uint32_t cand[] = { 1u << 17, 1u << 16, 1u << 15, 1u << 14,
                                     1u << 13, 1u << 12, 1u << 11, 1u << 10,
                                     1u << 9,  1u << 8 };
    int ok = 0;
    for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        uint32_t rc = cand[i];
        if (g_cfg.strict_48bit && rc > (1u << 16))
            continue;                       /* 严格模式预留规模先截到 2^16 */
        uint64_t a = mmap_arena(rc, g_cfg.strict_48bit);
        if (!a)
            continue;
        /* 进程自身映射余量探针 */
        void *probe = mmap(NULL, 64UL << 20, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (probe == MAP_FAILED) {
            munmap((void *)(uintptr_t)a, (size_t)rc << 32);
            continue;
        }
        munmap(probe, 64UL << 20);
        vamap_arena = a;
        g_region_count = rc;
        g_arena_bytes = (size_t)rc << 32;
        ok = 1;
        break;
    }
    if (!ok) {
        vamap_shutdown();
        return -2;
    }

    /* 可分配 region 号上界（严格 48 位模式叠加 2^16-1 与 arena 上界，§2.3） */
    g_region_limit = g_region_count - 1;
    if (g_cfg.strict_48bit) {
        uint64_t room = (1ULL << 48) - vamap_arena;
        uint32_t lim = (uint32_t)((room >> 32) - 1);   /* floor((2^48-arena)/4GiB)-1 */
        if (lim > (1u << 16) - 1)
            lim = (1u << 16) - 1;
        if (lim > g_region_count - 1)
            lim = g_region_count - 1;
        if ((int)lim < 1) {
            /* arena 距 2^48 不足 8 GiB：连 region 1 都无法容纳 */
            vamap_shutdown();
            return -3;
        }
        g_region_limit = lim;
    }

    /* alias 池 */
    g_pool_size = g_cfg.alias_pool_size;
    if (g_pool_size > 0) {
        if (g_pool_size >= g_region_count || g_pool_size > g_region_limit) {
            vamap_shutdown();
            return -4;
        }
        g_pool_rr = 0;
    }

    /* 空闲链（region 0 保留；池区间不进空闲链），LIFO 升序弹出 */
    g_refcnt = calloc(g_region_count, sizeof(uint32_t));
    g_free_stack = calloc(g_region_count, sizeof(uint32_t));
    g_stripes = calloc(g_cfg.lock_stripes, sizeof(stripe_t));
    if (!g_refcnt || !g_free_stack || !g_stripes) {
        vamap_shutdown();
        return -5;
    }
    g_free_top = 0;
    for (uint32_t r = g_region_limit; r > g_pool_size; r--)
        g_free_stack[g_free_top++] = r;

    if (g_cfg.latency_mode == VAMAP_LATENCY_SLEEP) {
        for (uint32_t i = 0; i < g_cfg.lock_stripes; i++)
            pthread_mutex_init(&g_stripes[i].u.mtx, NULL);
    } else {
        for (uint32_t i = 0; i < g_cfg.lock_stripes; i++)
            g_stripes[i].u.spin.f = 0;
    }

    calibrate_tsc();
    vamap_reset_stats();
    g_vva_bump = 0;
    g_vva_nfree = 0;

    snprintf(g_backing, sizeof(g_backing), "%s", backing);
    g_init = 1;
    return 0;
}

void vamap_shutdown(void)
{
    if (vamap_table) {
        munmap(vamap_table, g_table_bytes);
        vamap_table = NULL;
    }
    if (vamap_arena) {
        munmap((void *)(uintptr_t)vamap_arena, g_arena_bytes);
        vamap_arena = 0;
    }
    if (g_stripes && g_init && g_cfg.latency_mode == VAMAP_LATENCY_SLEEP) {
        for (uint32_t i = 0; i < g_cfg.lock_stripes; i++)
            pthread_mutex_destroy(&g_stripes[i].u.mtx);
    }
    free(g_refcnt);     g_refcnt = NULL;
    free(g_free_stack); g_free_stack = NULL;
    free(g_stripes);    g_stripes = NULL;
    g_free_top = 0;
    g_pool_size = 0;
    g_region_count = 0;
    g_table_bytes = 0;
    g_arena_bytes = 0;
    g_init = 0;
}

vamap_info_t vamap_get_info(void)
{
    vamap_info_t info;
    memset(&info, 0, sizeof(info));
    info.region_count    = g_region_count;
    info.region_limit    = g_region_limit;
    info.alias_pool_size = g_pool_size;
    info.arena_base      = vamap_arena;
    snprintf(info.table_backing, sizeof(info.table_backing), "%s", g_backing);
    return info;
}
