/*
 * patterns.c — T1–T15 访存内核与表状态控制（test.md §4.2/§6）。
 *
 * 约定：
 *  - mode 0 = B0（直接 VA 访存内核，overhead 分母）；
 *    mode 1 = B1（转换 + 0% 无效，纯快路径）；mode 2 = B2（矩阵无效比例与时延）。
 *  - B1/B2 使用 design.md §5.6 的 volatile 融合宏（vload64/vstore64），每次
 *    ld/st 各执行一次转换，禁止 CSE 与循环提升。
 *  - 随机/不规则 pattern 的 B0 通过 va_of_idx[] 取每步 VA（replay 预计算，
 *    列表 load 计入基线）；规则 pattern 用相同的指针算术。
 *  - 固定 seq_seed：replay 一次得到 touched 集与 va_of_idx，各 rep 复用；
 *    每 rep 前按 §4.2 diff 重置表状态（走 vamap_map_destroy 回收 region），
 *    保证瞬态行为可复现。
 */
#define _GNU_SOURCE
#include "bench.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GOLDEN 0x9E3779B97F4A7C15ULL

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

uint64_t xs64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

uint32_t pow2_floor(uint32_t v)
{
    if (v == 0)
        return 0;
    while (v & (v - 1))
        v &= v - 1;
    return v;
}

void *bench_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "bench: out of memory (%zu bytes)\n", n);
        exit(2);
    }
    return p;
}

static inline double u64_to_d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t d_to_u64(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline uint32_t log2i(uint32_t v)
{
    uint32_t r = 0;
    while ((1u << (r + 1)) <= v)
        r++;
    return r;
}

/* ------------------------------------------------------------------ */
/* 运行时控制：按需（池大小/时延模式）重建 runtime；状态重置一律 latency 0 */
/* ------------------------------------------------------------------ */

static uint32_t s_cur_pool = UINT32_MAX;
static int      s_cur_lmode = -1;

static void ensure_runtime(run_ctx *rc, uint32_t pool)
{
    if (vamap_table && s_cur_pool == pool && s_cur_lmode == rc->p.latency_mode) {
        vamap_set_create_latency_ns(0);
        return;
    }
    if (vamap_table)
        vamap_shutdown();
    vamap_cfg c;
    memset(&c, 0, sizeof(c));
    c.lock_stripes = 4096;
    c.alias_pool_size = pool;
    c.latency_mode = rc->p.latency_mode;
    c.create_latency_ns = 0;
    if (vamap_init(&c) != 0) {
        fprintf(stderr, "bench: vamap_init failed (pool=%u)\n", pool);
        exit(2);
    }
    s_cur_pool = pool;
    s_cur_lmode = rc->p.latency_mode;
}

static void alloc_arrays(run_ctx *rc)
{
    rc->va_of_idx = bench_xmalloc((size_t)rc->M * 8);
    rc->touched   = bench_xmalloc(rc->M);
    rc->invalid   = bench_xmalloc(rc->M);
    rc->cur_valid = bench_xmalloc(rc->M);
    memset(rc->va_of_idx, 0, (size_t)rc->M * 8);
    memset(rc->touched, 0, rc->M);
    memset(rc->invalid, 0, rc->M);
    memset(rc->cur_valid, 0, rc->M);
}

/* replay 中的一个触达：标记 touched，无效则 latency 0 创建并记录 VA */
static inline void replay_touch(run_ctx *rc, uint32_t idx)
{
    rc->touched[idx] = 1;
    if (!rc->cur_valid[idx]) {
        if (vamap_map_create(idx) != 0) {
            fprintf(stderr, "bench: replay create failed (idx=%u, region exhausted?)\n", idx);
            exit(2);
        }
        rc->cur_valid[idx] = 1;
        rc->va_of_idx[idx] = vamap_table[idx];
    }
}

static void count_touched(run_ctx *rc)
{
    rc->n_touched = 0;
    for (uint32_t i = 0; i < rc->M; i++)
        if (rc->touched[i])
            rc->n_touched++;
}

/* §4.2：初始无效集合（rand：触及集合内随机精确选 round(ratio×|T|) 个；
 * target：按确定规则按序选） */
void bench_build_invalid(run_ctx *rc)
{
    uint32_t target = (uint32_t)(rc->p.invalid_ratio * rc->n_touched + 0.5);
    if (target > rc->n_touched)
        target = rc->n_touched;
    if (rc->p.scope_rand && target > 0 && target < rc->n_touched) {
        uint64_t s = rc->seq_seed ^ GOLDEN;
        uint32_t remaining = rc->n_touched, need = target;
        for (uint32_t idx = 0; idx < rc->M; idx++) {
            if (!rc->touched[idx])
                continue;
            double p = (double)need / (double)remaining;
            if ((double)(xs64(&s) >> 11) * (1.0 / 9007199254740992.0) < p) {
                rc->invalid[idx] = 1;
                need--;
            }
            remaining--;
        }
    } else {
        uint32_t c = 0;
        for (uint32_t idx = 0; idx < rc->M && c < target; idx++)
            if (rc->touched[idx]) {
                rc->invalid[idx] = 1;
                c++;
            }
    }
    rc->n_invalid = 0;
    for (uint32_t idx = 0; idx < rc->M; idx++)
        if (rc->invalid[idx] && rc->touched[idx])
            rc->n_invalid++;
}

/* §4.2：按目标状态 diff 重置表状态（走 vamap_map_destroy 回收 region，
 * 不得只把 entry 置 0），状态重置一律 latency 0，并清零 stats。
 * cur_valid 以表项硬件真值同步（kernel 期间的 destroy 不维护软件记账） */
void bench_diff_state_target(run_ctx *rc, const uint8_t *target)
{
    vamap_set_create_latency_ns(0);
    for (uint32_t idx = 0; idx < rc->M; idx++) {
        rc->cur_valid[idx] = vamap_table[idx] != 0;
        if (target[idx] && !rc->cur_valid[idx]) {
            vamap_map_create(idx);
            rc->cur_valid[idx] = 1;
        } else if (!target[idx] && rc->cur_valid[idx]) {
            vamap_map_destroy(idx);
            rc->cur_valid[idx] = 0;
        }
    }
    vamap_set_create_latency_ns(rc->p.latency_ns);
    vamap_reset_stats();
}

/* pattern 通用 reset_phase：phase 1 → 全 touched 有效（B0/B1）；
 * phase 2 → touched \ invalid（B0/B2） */
static void reset_phase_generic(run_ctx *rc, int phase)
{
    uint8_t *tgt = bench_xmalloc(rc->M);
    for (uint32_t idx = 0; idx < rc->M; idx++)
        tgt[idx] = rc->touched[idx] && (phase == 1 || !rc->invalid[idx]);
    bench_diff_state_target(rc, tgt);
    free(tgt);
}

/* 预热：以 replay 记录的 va_of_idx 为准提交 touched 表项的数据页
 * （每 idx bytes 字节窗口、步长 4KiB 各写 1 字节）；缺页计数单列（§9.2） */
static void warm_pages(run_ctx *rc, size_t bytes_per_idx)
{
    for (uint32_t idx = 0; idx < rc->M; idx++) {
        if (!rc->touched[idx] || !rc->va_of_idx[idx])
            continue;
        uint64_t base = rc->va_of_idx[idx];
        for (size_t off = 0; off < bytes_per_idx; off += 4096)
            *(volatile uint8_t *)(uintptr_t)(base + off) = 1;
    }
}

static void warm_none(run_ctx *rc, int phase) { (void)rc; (void)phase; }

/* 通用预期 creates：phase 1 = 0；phase 2 = invalid∩touched（§9.3） */
static uint64_t expected_generic(run_ctx *rc, int phase)
{
    return phase == 2 ? rc->n_invalid : 0;
}

/* ------------------------------------------------------------------ */
/* pattern 私有数据                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    /* T1 */
    uint64_t  t1_W;
    uint8_t  *t1_b0;
    /* T3–T6 / T15 */
    uint32_t  st_block, st_block_elems, st_sh, st_n_elem;
    double    st_q;
    double   *st_b0a, *st_b0b, *st_b0c;
    /* T7 */
    uint32_t  t7_S;
    /* T10 */
    uint32_t  t10_W;
    /* T8 */
    uint32_t  t8_nper, t8_b0n;
    uint32_t *t8_idxmap, *t8_next;
    uint64_t  t8_head_vva, t8_head_b0;
    uint64_t *t8_b0;
    /* T2 */
    uint32_t  t2_dw;
    /* T9 */
    double   *t9_cdf;
    uint32_t  t9_K;
    /* T12 */
    uint32_t  t12_K, t12_W, t12_ops_eff;
    /* T13 */
    uint8_t  *t13_b0;
    /* T14 */
    uint32_t  t14_data, t14_nlist;
    uint32_t *t14_list;
    uint8_t  *t14_b0;
} priv_t;

/* ------------------------------------------------------------------ */
/* T1 热点相邻访问（表项 L1 命中）——4 份等价表项变体（§4.2）             */
/* ------------------------------------------------------------------ */

static int t1_setup(run_ctx *rc)
{
    priv_t *pr = bench_xmalloc(sizeof(priv_t));
    memset(pr, 0, sizeof(*pr));
    rc->priv = pr;
    ensure_runtime(rc, rc->p.alias ? 64 : 0);
    rc->M = 4;
    alloc_arrays(rc);
    for (uint32_t i = 0; i < rc->M; i++)
        replay_touch(rc, i);
    count_touched(rc);
    bench_build_invalid(rc);
    pr->t1_W = rc->p.var_u ? rc->p.var_u : 4096;
    pr->t1_b0 = bench_xmalloc(rc->M * pr->t1_W);
    memset(pr->t1_b0, 0, rc->M * pr->t1_W);
    return 0;
}

static void t1_kernel(run_ctx *rc, int mode, int tid, int nthreads)
{
    (void)tid; (void)nthreads;
    priv_t *pr = rc->priv;
    uint64_t W = pr->t1_W, elems = W / 8;
    uint64_t ops = rc->p.ops & ~1ULL;     /* 每迭代 A[i]+=c 含 1R+1W = 2 次访存 */
    uint64_t iters = ops / 2;
    volatile uint64_t sink = 0;
    if (mode == 0) {
        uint8_t *b = pr->t1_b0;
        for (uint64_t i = 0; i < iters; i++) {
            uint64_t off = ((i >> 2) & (elems - 1)) * 8;
            volatile uint64_t *a = (volatile uint64_t *)(b + (i & 3) * W + off);
            *a += 1;
            sink += *a;
        }
    } else {
        for (uint64_t i = 0; i < iters; i++) {
            uint64_t vva = ((uint64_t)(i & 3) << 32) |
                           (((i >> 2) & (elems - 1)) * 8);
            vstore64(vva, vload64(vva) + 1);
        }
    }
    __atomic_add_fetch(&rc->ops_actual, ops, __ATOMIC_RELAXED);
    __atomic_add_fetch(&rc->sink, (uint64_t)sink, __ATOMIC_RELAXED);
}

static void t1_warm(run_ctx *rc, int phase)
{
    (void)phase;
    priv_t *pr = rc->priv;
    warm_pages(rc, (size_t)pr->t1_W);
}

/* ------------------------------------------------------------------ */
/* T2 随机全空间访问（表项 cache miss），DATA_WINDOW 固定 4KiB，R:W=2:1  */
/* ------------------------------------------------------------------ */

static int t2_setup(run_ctx *rc)
{
    priv_t *pr = bench_xmalloc(sizeof(priv_t));
    memset(pr, 0, sizeof(*pr));
    rc->priv = pr;
    uint32_t M = 1u << 20;
    if (rc->p.invalid_ratio > 0) {        /* §4.4 自适应缩规模 */
        uint32_t cap = pow2_floor((uint32_t)(rc->p.max_creates / rc->p.invalid_ratio));
        if (cap >= 1 && cap < M)
            M = cap;
    }
    ensure_runtime(rc, rc->p.alias ? 64 : 0);
    if (!rc->p.alias && M > vamap_get_info().region_limit)
        ensure_runtime(rc, 64);           /* 全空间形态必须 alias 池预填（§4.2） */
    pr->t2_dw = rc->p.var_u2 ? (uint32_t)rc->p.var_u2 : 4096;
    rc->M = M;
    alloc_arrays(rc);
    for (int tid = 0; tid < rc->p.nthreads; tid++) {
        uint64_t s = rc->seq_seed ^ GOLDEN * (uint64_t)(tid + 1);
        for (uint64_t i = 0; i < rc->p.ops; i++) {
            uint64_t x = xs64(&s);
            replay_touch(rc, (uint32_t)(x & (M - 1)));
        }
    }
    count_touched(rc);
    bench_build_invalid(rc);
    return 0;
}

static void t2_kernel(run_ctx *rc, int mode, int tid, int nthreads)
{
    (void)nthreads;
    priv_t *pr = rc->priv;
    uint32_t M = rc->M;
    const uint64_t dw = pr->t2_dw;        /* DATA_WINDOW（默认 4KiB，可配 64M 变体） */
    uint64_t s = rc->seq_seed ^ GOLDEN * (uint64_t)(tid + 1);
    uint64_t ops = rc->p.ops;
    volatile uint64_t sink = 0;
    if (mode == 0) {
        const uint64_t *va = rc->va_of_idx;
        for (uint64_t i = 0; i < ops; i++) {
            uint64_t x = xs64(&s);
            uint32_t idx = (uint32_t)(x & (M - 1));
            uint64_t off = ((x >> 32) & (dw - 1)) & ~7ULL;
            uint64_t a = va[idx] | off;
            if (((x >> 48) & 3) < 2)
                sink += *(volatile uint64_t *)a;
            else
                *(volatile uint64_t *)a = sink;
        }
    } else {
        for (uint64_t i = 0; i < ops; i++) {
            uint64_t x = xs64(&s);
            uint32_t idx = (uint32_t)(x & (M - 1));
            uint64_t off = ((x >> 32) & (dw - 1)) & ~7ULL;
            uint64_t vva = ((uint64_t)idx << 32) | off;
            if (((x >> 48) & 3) < 2)
                sink += vload64(vva);
            else
                vstore64(vva, sink);
        }
    }
    __atomic_add_fetch(&rc->ops_actual, ops, __ATOMIC_RELAXED);
    __atomic_add_fetch(&rc->sink, (uint64_t)sink, __ATOMIC_RELAXED);
}

static void t2_warm(run_ctx *rc, int phase)
{
    (void)phase;
    priv_t *pr = rc->priv;
    /* alias 池生效时按 region 预触（多个 idx 共享 region，逐 idx 预触会放大）；
     * 非 alias 路径按每 idx 数据窗口预触 */
    if (vamap_get_info().alias_pool_size > 0) {
        uint32_t pool = vamap_get_info().alias_pool_size;
        for (uint32_t r = 1; r <= pool; r++)
            for (uint64_t off = 0; off < pr->t2_dw; off += 4096)
                *(volatile uint8_t *)(uintptr_t)(vamap_arena +
                                                 ((uint64_t)r << 32) + off) = 1;
    } else {
        warm_pages(rc, pr->t2_dw);
    }
}

/* ------------------------------------------------------------------ */
/* T3–T6 STREAM-M 分块变体 / T15 hoist 对比                             */
/* ------------------------------------------------------------------ */

static int stream_accesses(int kind) { return (kind == 3 || kind == 4) ? 2 : 3; }

static int stream_setup(run_ctx *rc, int kind)
{
    priv_t *pr = bench_xmalloc(sizeof(priv_t));
    memset(pr, 0, sizeof(*pr));
    rc->priv = pr;
    ensure_runtime(rc, rc->p.alias ? 64 : 0);
    uint32_t M = rc->p.var_u ? (uint32_t)rc->p.var_u : 256;
    if (rc->p.invalid_ratio > 0) {        /* §4.4：D = 分块数 M */
        uint32_t cap = pow2_floor((uint32_t)(rc->p.max_creates / rc->p.invalid_ratio));
        if (cap >= 1 && cap < M)
            M = cap;
    }
    uint64_t iters = rc->p.ops / (uint64_t)stream_accesses(kind);
    uint64_t want_block = rc->p.var_u2 ? rc->p.var_u2 : (1ULL << 20);
    /* 分块元素数：pow2 且 ≤ iters/M（保证轮转覆盖全部分块）且 ≤ want_block/8 */
    uint64_t cap_elems = iters / (M ? M : 1);
    uint64_t want_elems = want_block / 8;
    uint32_t block_elems = (uint32_t)pow2_floor((uint32_t)(cap_elems < want_elems ?
                                                           cap_elems : want_elems));
    if (block_elems < 8)
        block_elems = 8;
    while (M > 1 && (uint64_t)M * block_elems > iters)
        M = pow2_floor(M / 2);
    pr->st_block = block_elems * 8;
    pr->st_block_elems = block_elems;
    pr->st_sh = log2i(block_elems);
    pr->st_n_elem = M * block_elems;
    pr->st_q = 2.0;
    rc->M = M;
    alloc_arrays(rc);
    for (uint32_t idx = 0; idx < M; idx++)
        replay_touch(rc, idx);
    count_touched(rc);
    bench_build_invalid(rc);
    (void)kind;
    size_t bytes = (size_t)M * pr->st_block;
    pr->st_b0a = bench_xmalloc(bytes);
    pr->st_b0b = bench_xmalloc(bytes);
    pr->st_b0c = bench_xmalloc(bytes);
    memset(pr->st_b0a, 0, bytes);
    memset(pr->st_b0b, 0, bytes);
    memset(pr->st_b0c, 0, bytes);
    return 0;
}

static void stream_warm(run_ctx *rc, int phase)
{
    (void)phase;
    priv_t *pr = rc->priv;
    for (uint32_t idx = 0; idx < rc->M; idx++) {
        if (!rc->touched[idx] || !rc->va_of_idx[idx])
            continue;
        uint64_t base = rc->va_of_idx[idx];
        for (size_t off = 0; off < (size_t)pr->st_block * 3; off += 4096)
            *(volatile uint8_t *)(uintptr_t)(base + off) = 1;
    }
}

/* kind: 3=Copy 4=Scale 5=Add 6/15=Triad；mode 2 且 kind==15 → hoist 变体 */
static void stream_kernel(run_ctx *rc, int mode, int tid, int nthreads, int kind)
{
    (void)tid; (void)nthreads;
    priv_t *pr = rc->priv;
    uint32_t ne = pr->st_n_elem, be = pr->st_block_elems, sh = pr->st_sh;
    uint64_t blk = pr->st_block;
    double q = pr->st_q;
    uint64_t acc = (uint64_t)stream_accesses(kind);
    uint64_t iters = rc->p.ops / acc;
    volatile uint64_t sink = 0;

    if (mode == 0) {
        double *A = pr->st_b0a, *B = pr->st_b0b, *C = pr->st_b0c;
        for (uint64_t k = 0; k < iters; k++) {
            uint32_t e = (uint32_t)(k % ne);
            switch (kind) {
            case 3: A[e] = B[e]; break;
            case 4: A[e] = q * C[e]; break;
            case 5: C[e] = A[e] + B[e]; break;
            default: A[e] = B[e] + q * C[e]; break;
            }
        }
        sink += d_to_u64(*(volatile double *)&A[(uint32_t)(iters % ne)]);
    } else if (kind == 15 && mode == 2) {
        /* T15 hoist：每分块一次 vamap_pin 提升查表，块内直接 VA 访存 */
        uint64_t bA = 0, bB = 0, bC = 0;
        uint32_t pj = 0xFFFFFFFFu;
        for (uint64_t k = 0; k < iters; k++) {
            uint32_t e = (uint32_t)(k % ne);
            uint32_t j = e >> sh, eo = e & (be - 1);
            if (j != pj) {
                bA = (uint64_t)(uintptr_t)vamap_pin(((uint64_t)j << 32) | 0);
                bB = (uint64_t)(uintptr_t)vamap_pin(((uint64_t)j << 32) | blk);
                bC = (uint64_t)(uintptr_t)vamap_pin(((uint64_t)j << 32) | 2 * blk);
                pj = j;
            }
            *(volatile uint64_t *)(uintptr_t)(bA | ((uint64_t)eo * 8)) =
                d_to_u64(u64_to_d(*(volatile uint64_t *)(uintptr_t)(bB | ((uint64_t)eo * 8))) +
                         q * u64_to_d(*(volatile uint64_t *)(uintptr_t)(bC | ((uint64_t)eo * 8))));
        }
        sink += *(volatile uint64_t *)(uintptr_t)(bA | 0);
    } else {
        for (uint64_t k = 0; k < iters; k++) {
            uint32_t e = (uint32_t)(k % ne);
            uint32_t j = e >> sh, eo = e & (be - 1);
            uint64_t vvaA = ((uint64_t)j << 32) | ((uint64_t)eo * 8);
            uint64_t vvaB = ((uint64_t)j << 32) | (blk + (uint64_t)eo * 8);
            uint64_t vvaC = ((uint64_t)j << 32) | (2 * blk + (uint64_t)eo * 8);
            switch (kind) {
            case 3:
                vstore64(vvaA, vload64(vvaB));
                break;
            case 4:
                vstore64(vvaA, d_to_u64(q * u64_to_d(vload64(vvaC))));
                break;
            case 5:
                vstore64(vvaC, d_to_u64(u64_to_d(vload64(vvaA)) +
                                        u64_to_d(vload64(vvaB))));
                break;
            default:
                vstore64(vvaA, d_to_u64(u64_to_d(vload64(vvaB)) +
                                        q * u64_to_d(vload64(vvaC))));
                break;
            }
        }
    }
    __atomic_add_fetch(&rc->ops_actual, iters * acc, __ATOMIC_RELAXED);
    __atomic_add_fetch(&rc->sink, (uint64_t)sink, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/* T7 步长访问（单 region 池，数据固定 64B 窗口）                        */
/* ------------------------------------------------------------------ */

static int t7_setup(run_ctx *rc)
{
    priv_t *pr = bench_xmalloc(sizeof(priv_t));
    memset(pr, 0, sizeof(*pr));
    rc->priv = pr;
    uint32_t S = rc->p.var_u ? (uint32_t)rc->p.var_u : 64;
    pr->t7_S = S;
    uint32_t M = 1u << 20;
    if (rc->p.invalid_ratio > 0) {        /* D = M/gcd(S,M)（§4.4） */
        uint64_t budget = rc->p.max_creates;
        uint64_t cap2 = (uint64_t)((double)budget * S / rc->p.invalid_ratio);
        uint64_t cap1 = (uint64_t)((double)budget / rc->p.invalid_ratio);
        uint32_t cap = cap2 >= S ? pow2_floor((uint32_t)cap2) :
                                   pow2_floor((uint32_t)cap1);
        if (cap >= 1 && cap < M)
            M = cap;
    }
    ensure_runtime(rc, 1);                /* 单 region 池（test.md §6 T7） */
    rc->M = M;
    alloc_arrays(rc);
    for (uint64_t i = 0; i < rc->p.ops; i++)
        replay_touch(rc, (uint32_t)((i * S) & (M - 1)));
    count_touched(rc);
    bench_build_invalid(rc);
    return 0;
}

static void t7_kernel(run_ctx *rc, int mode, int tid, int nthreads)
{
    (void)tid; (void)nthreads;
    priv_t *pr = rc->priv;
    uint32_t M = rc->M, S = pr->t7_S;
    uint64_t ops = rc->p.ops;
    volatile uint64_t sink = 0;
    if (mode == 0) {
        const uint64_t *va = rc->va_of_idx;
        for (uint64_t i = 0; i < ops; i++) {
            uint64_t a = va[(uint32_t)((i * S) & (M - 1))] | ((i & 7) * 8);
            sink += *(volatile uint64_t *)a;
        }
    } else {
        for (uint64_t i = 0; i < ops; i++) {
            uint64_t vva = ((uint64_t)((i * S) & (M - 1)) << 32) | ((i & 7) * 8);
            sink += vload64(vva);
        }
    }
    __atomic_add_fetch(&rc->ops_actual, ops, __ATOMIC_RELAXED);
    __atomic_add_fetch(&rc->sink, (uint64_t)sink, __ATOMIC_RELAXED);
}

static void t7_warm(run_ctx *rc, int phase)
{
    (void)phase;
    warm_pages(rc, 64);
}

/* ------------------------------------------------------------------ */
/* T8 指针追逐（alias 池 + 槽位内容复制无关，任何 idx→region 映射下链成立）*/
/* ------------------------------------------------------------------ */

static int t8_setup(run_ctx *rc)
{
    priv_t *pr = bench_xmalloc(sizeof(priv_t));
    memset(pr, 0, sizeof(*pr));
    rc->priv = pr;
    ensure_runtime(rc, 64);
    uint32_t nper = rc->p.var_u ? (uint32_t)rc->p.var_u : 4096;
    if (rc->p.invalid_ratio > 0) {
        uint32_t cap = pow2_floor((uint32_t)(rc->p.max_creates / rc->p.invalid_ratio));
        if (cap >= 1 && cap < nper)
            nper = cap;
    }
    pr->t8_nper = nper;
    rc->M = nper;
    alloc_arrays(rc);

    uint64_t s = rc->seq_seed ^ GOLDEN;
    uint32_t *ord = bench_xmalloc(nper * 4);
    uint32_t *idxmap = bench_xmalloc(nper * 4);
    pr->t8_idxmap = idxmap;
    pr->t8_next = bench_xmalloc(nper * 4);
    for (uint32_t i = 0; i < nper; i++) {
        ord[i] = i;
        idxmap[i] = i;
    }
    for (uint32_t i = nper - 1; i > 0; i--) {
        uint32_t j = (uint32_t)(xs64(&s) % (i + 1));
        uint32_t t = ord[i]; ord[i] = ord[j]; ord[j] = t;
        j = (uint32_t)(xs64(&s) % (i + 1));
        t = idxmap[i]; idxmap[i] = idxmap[j]; idxmap[j] = t;
    }
    for (uint32_t i = 0; i < nper; i++)
        pr->t8_next[ord[i]] = ord[(i + 1) % nper];
    pr->t8_head_vva = ((uint64_t)idxmap[ord[0]] << 32) | ((uint64_t)ord[0] * 64);

    /* 预写全部池 region 的槽位：同一份链内容 → 数据别名不影响链语义 */
    uint32_t npool = vamap_get_info().alias_pool_size ? : 64;
    for (uint32_t r = 1; r <= npool; r++) {
        uint64_t base = vamap_arena + ((uint64_t)r << 32);
        for (uint32_t sp = 0; sp < nper; sp++) {
            uint32_t nx = pr->t8_next[sp];
            *(volatile uint64_t *)(uintptr_t)(base + (uint64_t)sp * 64) =
                ((uint64_t)idxmap[nx] << 32) | ((uint64_t)nx * 64);
        }
    }

    /* replay：沿链触达全部 idx（latency 0 创建） */
    uint32_t cur = ord[0];
    for (uint32_t h = 0; h < nper; h++) {
        replay_touch(rc, idxmap[cur]);
        cur = pr->t8_next[cur];
    }
    count_touched(rc);
    bench_build_invalid(rc);

    /* B0：同构直接 VA 链，足迹 64×nper×64B（与 B1/B2 池足迹对齐） */
    pr->t8_b0n = 64 * nper;
    pr->t8_b0 = bench_xmalloc((size_t)pr->t8_b0n * 64);
    uint32_t *ord2 = bench_xmalloc(pr->t8_b0n * 4);
    for (uint32_t i = 0; i < pr->t8_b0n; i++)
        ord2[i] = i;
    for (uint32_t i = pr->t8_b0n - 1; i > 0; i--) {
        uint32_t j = (uint32_t)(xs64(&s) % (i + 1));
        uint32_t t = ord2[i]; ord2[i] = ord2[j]; ord2[j] = t;
    }
    for (uint32_t i = 0; i < pr->t8_b0n; i++) {
        uint32_t nx = ord2[(i + 1) % pr->t8_b0n];
        pr->t8_b0[(size_t)ord2[i] * 8] =
            (uint64_t)(uintptr_t)&pr->t8_b0[(size_t)nx * 8];
    }
    pr->t8_head_b0 = (uint64_t)(uintptr_t)&pr->t8_b0[(size_t)ord2[0] * 8];
    free(ord);
    free(ord2);
    return 0;
}

static void t8_kernel(run_ctx *rc, int mode, int tid, int nthreads)
{
    (void)tid; (void)nthreads;
    priv_t *pr = rc->priv;
    uint64_t hops = rc->p.ops;
    volatile uint64_t sink = 0;
    if (mode == 0) {
        uint64_t p = pr->t8_head_b0;
        for (uint64_t i = 0; i < hops; i++)
            p = *(volatile uint64_t *)(uintptr_t)p;
        sink += p;
    } else {
        uint64_t p = pr->t8_head_vva;
        for (uint64_t i = 0; i < hops; i++)
            p = vload64(p);
        sink += p;
    }
    __atomic_add_fetch(&rc->ops_actual, hops, __ATOMIC_RELAXED);
    __atomic_add_fetch(&rc->sink, (uint64_t)sink, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/* T9 热/冷分布（Zipf）                                                 */
/* ------------------------------------------------------------------ */

static void t9_build_cdf(priv_t *pr, uint32_t K, double s)
{
    pr->t9_K = K;
    free(pr->t9_cdf);
    pr->t9_cdf = bench_xmalloc((size_t)K * 8);
    double sum = 0.0;
    for (uint32_t k = 0; k < K; k++)
        sum += 1.0 / pow((double)k + 1.0, s);
    double c = 0.0;
    for (uint32_t k = 0; k < K; k++) {
        c += 1.0 / pow((double)k + 1.0, s) / sum;
        pr->t9_cdf[k] = c;
    }
    pr->t9_cdf[K - 1] = 1.0;
}

static inline uint32_t t9_key(const priv_t *pr, double u)
{
    uint32_t lo = 0, hi = pr->t9_K - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (pr->t9_cdf[mid] < u)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int t9_setup(run_ctx *rc)
{
    priv_t *pr = bench_xmalloc(sizeof(priv_t));
    memset(pr, 0, sizeof(*pr));
    rc->priv = pr;
    double s = rc->p.var_u ? (double)rc->p.var_u / 1000.0 : 0.9;
    uint32_t K = 1u << 16;
    if (rc->p.invalid_ratio > 0) {
        uint32_t cap = pow2_floor((uint32_t)(rc->p.max_creates / rc->p.invalid_ratio));
        if (cap >= 1 && cap < K)
            K = cap;
    }
    ensure_runtime(rc, 64);               /* alias 池预填避免 region 耗尽（§6 T9） */
    t9_build_cdf(pr, K, s);
    rc->M = K;
    alloc_arrays(rc);
    for (int tid = 0; tid < rc->p.nthreads; tid++) {
        uint64_t sd = rc->seq_seed ^ GOLDEN * (uint64_t)(tid + 1);
        for (uint64_t i = 0; i < rc->p.ops; i++) {
            uint64_t x = xs64(&sd);
            double u = (double)(x >> 11) * (1.0 / 9007199254740992.0);
            replay_touch(rc, t9_key(pr, u));
        }
    }
    count_touched(rc);
    bench_build_invalid(rc);
    return 0;
}

static void t9_kernel(run_ctx *rc, int mode, int tid, int nthreads)
{
    (void)nthreads;
    priv_t *pr = rc->priv;
    const uint64_t dw = 4096;
    uint64_t s = rc->seq_seed ^ GOLDEN * (uint64_t)(tid + 1);
    uint64_t ops = rc->p.ops;
    volatile uint64_t sink = 0;
    if (mode == 0) {
        const uint64_t *va = rc->va_of_idx;
        for (uint64_t i = 0; i < ops; i++) {
            uint64_t x = xs64(&s);
            uint32_t key = t9_key(pr, (double)(x >> 11) * (1.0 / 9007199254740992.0));
            uint64_t off = ((uint64_t)key & (dw / 8 - 1)) * 8;
            uint64_t a = va[key] | off;
            if (((x >> 52) & 3) < 2)
                sink += *(volatile uint64_t *)a;
            else
                *(volatile uint64_t *)a = sink;
        }
    } else {
        for (uint64_t i = 0; i < ops; i++) {
            uint64_t x = xs64(&s);
            uint32_t key = t9_key(pr, (double)(x >> 11) * (1.0 / 9007199254740992.0));
            uint64_t off = ((uint64_t)key & (dw / 8 - 1)) * 8;
            uint64_t vva = ((uint64_t)key << 32) | off;
            if (((x >> 52) & 3) < 2)
                sink += vload64(vva);
            else
                vstore64(vva, sink);
        }
    }
    __atomic_add_fetch(&rc->ops_actual, ops, __ATOMIC_RELAXED);
    __atomic_add_fetch(&rc->sink, (uint64_t)sink, __ATOMIC_RELAXED);
}

static void t9_warm(run_ctx *rc, int phase)
{
    (void)phase;
    warm_pages(rc, 4096);
}

/* ------------------------------------------------------------------ */
/* T10 表工作集扫描（数据侧固定小窗口）                                  */
/* ------------------------------------------------------------------ */

static int t10_setup(run_ctx *rc)
{
    priv_t *pr = bench_xmalloc(sizeof(priv_t));
    memset(pr, 0, sizeof(*pr));
    rc->priv = pr;
    uint32_t W = rc->p.var_u ? (uint32_t)rc->p.var_u : 4096;
    if (rc->p.invalid_ratio > 0) {
        uint32_t cap = pow2_floor((uint32_t)(rc->p.max_creates / rc->p.invalid_ratio));
        if (cap >= 1 && cap < W)
            W = cap;                      /* §4.4：M=W 截断 */
    }
    if (W < 4)
        W = 4;                            /* |T|<4 → 4 份等价表项变体（§4.2） */
    uint32_t pool = rc->p.alias ? 64 : 0;
    ensure_runtime(rc, pool);
    if (!rc->p.alias && W > vamap_get_info().region_limit)
        ensure_runtime(rc, 64);           /* W 超过可用 region 数时 alias 预填 */
    pr->t10_W = W;
    rc->M = W;
    alloc_arrays(rc);
    for (int tid = 0; tid < rc->p.nthreads; tid++) {
        uint64_t s = rc->seq_seed ^ GOLDEN * (uint64_t)(tid + 1);
        for (uint64_t i = 0; i < rc->p.ops; i++) {
            uint64_t x = xs64(&s);
            replay_touch(rc, (uint32_t)(x & (W - 1)));
        }
    }
    count_touched(rc);
    bench_build_invalid(rc);
    return 0;
}

static void t10_kernel(run_ctx *rc, int mode, int tid, int nthreads)
{
    (void)nthreads;
    priv_t *pr = rc->priv;
    uint32_t W = pr->t10_W;
    uint64_t s = rc->seq_seed ^ GOLDEN * (uint64_t)(tid + 1);
    uint64_t ops = rc->p.ops;
    volatile uint64_t sink = 0;
    if (mode == 0) {
        const uint64_t *va = rc->va_of_idx;
        for (uint64_t i = 0; i < ops; i++) {
            uint64_t x = xs64(&s);
            uint64_t a = va[(uint32_t)(x & (W - 1))] | 32;
            sink += *(volatile uint64_t *)a;
        }
    } else {
        for (uint64_t i = 0; i < ops; i++) {
            uint64_t x = xs64(&s);
            uint64_t vva = ((uint64_t)(x & (W - 1)) << 32) | 32;
            sink += vload64(vva);
        }
    }
    __atomic_add_fetch(&rc->ops_actual, ops, __ATOMIC_RELAXED);
    __atomic_add_fetch(&rc->sink, (uint64_t)sink, __ATOMIC_RELAXED);
}

static void t10_warm(run_ctx *rc, int phase)
{
    (void)phase;
    warm_pages(rc, 64);
}

/* ------------------------------------------------------------------ */
/* T11 首触风暴（多线程创建竞争）：shared=4 表项 / disjoint=每线程 16 个  */
/* ------------------------------------------------------------------ */

static int t11_setup(run_ctx *rc)
{
    priv_t *pr = bench_xmalloc(sizeof(priv_t));
    memset(pr, 0, sizeof(*pr));
    rc->priv = pr;
    ensure_runtime(rc, rc->p.alias ? 64 : 0);
    uint32_t setn = rc->p.idx_shared ? 4 : 16;
    pr->t12_W = setn;
    rc->M = rc->p.idx_shared ? setn : setn * (uint32_t)rc->p.nthreads;
    alloc_arrays(rc);
    for (uint32_t idx = 0; idx < rc->M; idx++)
        replay_touch(rc, idx);
    count_touched(rc);
    /* 目标式失效注入：shared 前 round(ratio×4) 个；disjoint 每线程切片
     * 前 round(ratio×16) 个（§4.2 的 4 份等价表项语义） */
    uint32_t ninv = (uint32_t)(rc->p.invalid_ratio * setn + 0.5);
    if (ninv > setn)
        ninv = setn;
    if (rc->p.idx_shared) {
        for (uint32_t k = 0; k < ninv; k++)
            rc->invalid[k] = 1;
    } else {
        for (int t = 0; t < rc->p.nthreads; t++)
            for (uint32_t k = 0; k < ninv; k++)
                rc->invalid[t * setn + k] = 1;
    }
    rc->n_invalid = rc->p.idx_shared ? ninv : ninv * (uint32_t)rc->p.nthreads;
    return 0;
}

static void t11_warm(run_ctx *rc, int phase)
{
    /* 预触各 idx 首页：T11 测多线程创建竞争（稳态），首触 THP 缺页
     * （~600µs/页）不属于被测量，留在 warm 阶段完成 */
    (void)phase;
    warm_pages(rc, 64);
}

static void t11_kernel(run_ctx *rc, int mode, int tid, int nthreads)
{
    (void)nthreads;
    priv_t *pr = rc->priv;
    uint32_t setn = pr->t12_W;
    volatile uint64_t sink = 0;
    uint64_t touches = 4096;
    if (rc->p.idx_shared) {
        for (uint64_t i = 0; i < touches; i++) {
            uint32_t idx = (uint32_t)(i & (setn - 1));
            if (mode == 0) {
                *(volatile uint64_t *)(uintptr_t)(rc->va_of_idx[idx]) = tid;
            } else {
                vstore64((uint64_t)idx << 32, (uint64_t)tid);
            }
        }
    } else {
        uint32_t base = (uint32_t)tid * setn;
        for (uint64_t i = 0; i < touches; i++) {
            uint32_t idx = base + (uint32_t)(i & (setn - 1));
            if (mode == 0) {
                *(volatile uint64_t *)(uintptr_t)(rc->va_of_idx[idx]) = tid;
            } else {
                vstore64((uint64_t)idx << 32, (uint64_t)tid);
            }
        }
    }
    __atomic_add_fetch(&rc->ops_actual, touches, __ATOMIC_RELAXED);
    __atomic_add_fetch(&rc->sink, (uint64_t)sink, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/* T12 销毁–重建 churn（持续无效压力）                                   */
/* ------------------------------------------------------------------ */

static int t12_setup(run_ctx *rc)
{
    priv_t *pr = bench_xmalloc(sizeof(priv_t));
    memset(pr, 0, sizeof(*pr));
    rc->priv = pr;
    uint32_t K = rc->p.var_u ? (uint32_t)rc->p.var_u : 256;
    uint32_t W = 64;
    uint32_t nth = (uint32_t)(rc->p.nthreads ? rc->p.nthreads : 1);
    uint32_t slice = W / nth ? W / nth : 1;
    if (K < slice)
        K = slice;                        /* 保证每个 destroy 后必有同 idx 重建 */
    pr->t12_K = K;
    pr->t12_W = W;
    ensure_runtime(rc, rc->p.alias ? 64 : 0);
    rc->M = W;
    alloc_arrays(rc);
    for (uint32_t idx = 0; idx < W; idx++)
        replay_touch(rc, idx);
    count_touched(rc);
    bench_build_invalid(rc);
    /* §4.4：只缩短测量窗口（每线程 ops 对齐到 K），使 churn ≤ max_creates */
    uint64_t per_thread = rc->p.ops / nth;
    per_thread = per_thread / K * K;
    uint64_t bud = rc->p.max_creates / nth;
    if (bud == 0)
        bud = 1;
    bud *= K;
    if (per_thread > bud)
        per_thread = bud;
    if (per_thread < K)
        per_thread = K;
    pr->t12_ops_eff = (uint32_t)per_thread;
    return 0;
}

static void t12_kernel(run_ctx *rc, int mode, int tid, int nthreads)
{
    priv_t *pr = rc->priv;
    uint32_t W = pr->t12_W, K = pr->t12_K;
    uint32_t slice = W / (uint32_t)nthreads;
    if (slice == 0)
        slice = 1;
    uint32_t base = (uint32_t)tid * slice;
    if (base >= W)
        base = W - 1, slice = 1;          /* 防越界（threads 不整除 W 时收尾线程） */
    if (base + slice > W)
        slice = W - base;
    uint64_t ops = pr->t12_ops_eff;
    volatile uint64_t sink = 0;
    uint32_t dj = 0;                      /* destroy 轮转序号：逐窗遍历切片内
                                             各 idx，保证每次 destroy 都有效且
                                             互不重复（K 为 slice 倍数时固定
                                             i%slice 会重复命中同一表项） */
    for (uint64_t i = 0; i < ops; i++) {
        uint32_t idx = base + (uint32_t)(i % slice);
        uint64_t off = (i & 7) * 8;
        if (mode == 0) {
            uint64_t a = rc->va_of_idx[idx] | off;
            sink += *(volatile uint64_t *)a;
        } else {
            uint64_t vva = ((uint64_t)idx << 32) | off;
            sink += vload64(vva);
        }
        /* 每 K 次访存销毁一个切片内轮转表项 → 后续访问重建（线程私有切片，
         * 遵守 §6.4 销毁纪律；destroy 与访问解耦但重建必然发生：K ≥ slice
         * 保证下个 destroy 前该 idx 已被访问过 ≥1 次） */
        if (mode == 2 && (i + 1) % K == 0)
            vamap_map_destroy(base + (dj++ % slice));
    }
    __atomic_add_fetch(&rc->ops_actual, ops, __ATOMIC_RELAXED);
    __atomic_add_fetch(&rc->sink, (uint64_t)sink, __ATOMIC_RELAXED);
}

static uint64_t t12_expected(run_ctx *rc, int phase)
{
    if (phase != 2)
        return 0;
    priv_t *pr = rc->priv;
    uint32_t nth = (uint32_t)(rc->p.nthreads ? rc->p.nthreads : 1);
    /* churn：每线程 ops_eff/K 次 destroy，除最后一次（无后续重建）外每次
     * destroy 均紧跟一次重建（slice ≤ K 保证） */
    uint64_t churn = (uint64_t)nth * (pr->t12_ops_eff / pr->t12_K);
    if (churn >= nth)
        churn -= nth;
    return (uint64_t)rc->n_invalid + churn;
}

static void t12_warm(run_ctx *rc, int phase)
{
    (void)phase;
    warm_pages(rc, 64);
}

/* ------------------------------------------------------------------ */
/* T13 读改写（RMW）：每 RMW 恰 1 次转换                                */
/* ------------------------------------------------------------------ */

static int t13_setup(run_ctx *rc)
{
    priv_t *pr = bench_xmalloc(sizeof(priv_t));
    memset(pr, 0, sizeof(*pr));
    rc->priv = pr;
    ensure_runtime(rc, rc->p.alias ? 64 : 0);
    rc->M = 4;
    alloc_arrays(rc);
    for (uint32_t i = 0; i < rc->M; i++)
        replay_touch(rc, i);
    count_touched(rc);
    bench_build_invalid(rc);
    pr->t13_b0 = bench_xmalloc(rc->M * 256);
    memset(pr->t13_b0, 0, rc->M * 256);
    return 0;
}

static void t13_kernel(run_ctx *rc, int mode, int tid, int nthreads)
{
    (void)tid; (void)nthreads;
    priv_t *pr = rc->priv;
    uint64_t ops = rc->p.ops;
    volatile uint64_t sink = 0;
    if (mode == 0) {
        for (uint64_t i = 0; i < ops; i++) {
            uint32_t idx = (uint32_t)(i & 3);
            uint64_t off = ((i >> 2) & 31) * 8;
            sink += (uint64_t)__atomic_fetch_add(
                (uint64_t *)(pr->t13_b0 + idx * 256 + off), 1, __ATOMIC_RELAXED);
        }
    } else {
        for (uint64_t i = 0; i < ops; i++) {
            uint32_t idx = (uint32_t)(i & 3);
            uint64_t off = ((i >> 2) & 31) * 8;
            /* 每 RMW 恰 1 次转换（优化形态） */
            sink += (uint64_t)__atomic_fetch_add(
                (uint64_t *)(uintptr_t)vva2va(((uint64_t)idx << 32) | off), 1,
                __ATOMIC_RELAXED);
        }
    }
    __atomic_add_fetch(&rc->ops_actual, ops, __ATOMIC_RELAXED);
    __atomic_add_fetch(&rc->sink, (uint64_t)sink, __ATOMIC_RELAXED);
}

static void t13_warm(run_ctx *rc, int phase)
{
    (void)phase;
    warm_pages(rc, 256);
}

/* ------------------------------------------------------------------ */
/* T14 gather / scatter（表热 + 数据冷）                                */
/* ------------------------------------------------------------------ */

static int t14_setup(run_ctx *rc)
{
    priv_t *pr = bench_xmalloc(sizeof(priv_t));
    memset(pr, 0, sizeof(*pr));
    rc->priv = pr;
    ensure_runtime(rc, rc->p.alias ? 64 : 0);
    rc->M = 4;
    uint32_t data = rc->p.var_u2 ? (uint32_t)rc->p.var_u2 : (4u << 20);
    pr->t14_data = data;
    alloc_arrays(rc);
    for (uint32_t i = 0; i < rc->M; i++)
        replay_touch(rc, i);
    count_touched(rc);
    bench_build_invalid(rc);
    pr->t14_nlist = 4096;
    pr->t14_list = bench_xmalloc(pr->t14_nlist * 4);
    uint64_t s = rc->seq_seed ^ GOLDEN;
    for (uint32_t k = 0; k < pr->t14_nlist; k++)
        pr->t14_list[k] = (uint32_t)(xs64(&s) % (data / 8));
    pr->t14_b0 = bench_xmalloc((size_t)rc->M * data);
    memset(pr->t14_b0, 0, (size_t)rc->M * data);
    return 0;
}

static void t14_kernel(run_ctx *rc, int mode, int tid, int nthreads)
{
    (void)tid; (void)nthreads;
    priv_t *pr = rc->priv;
    uint32_t data = pr->t14_data;
    uint64_t ops = rc->p.ops & ~1ULL;
    uint64_t ng = ops / 2, ns = ops - ng;
    volatile uint64_t sink = 0;
    if (mode == 0) {
        uint8_t *b = pr->t14_b0;
        for (uint64_t i = 0; i < ng; i++) {
            uint64_t a = (i & 3) * data + (uint64_t)pr->t14_list[i & (pr->t14_nlist - 1)] * 8;
            sink += *(volatile uint64_t *)(b + a);
        }
        for (uint64_t i = 0; i < ns; i++) {
            uint64_t a = (i & 3) * data + (uint64_t)pr->t14_list[i & (pr->t14_nlist - 1)] * 8;
            *(volatile uint64_t *)(b + a) = sink;
        }
    } else {
        for (uint64_t i = 0; i < ng; i++) {
            uint64_t vva = ((uint64_t)(i & 3) << 32) |
                           (uint64_t)pr->t14_list[i & (pr->t14_nlist - 1)] * 8;
            sink += vload64(vva);         /* gather */
        }
        for (uint64_t i = 0; i < ns; i++) {
            uint64_t vva = ((uint64_t)(i & 3) << 32) |
                           (uint64_t)pr->t14_list[i & (pr->t14_nlist - 1)] * 8;
            vstore64(vva, sink);          /* scatter */
        }
    }
    __atomic_add_fetch(&rc->ops_actual, ops, __ATOMIC_RELAXED);
    __atomic_add_fetch(&rc->sink, (uint64_t)sink, __ATOMIC_RELAXED);
}

static void t14_warm(run_ctx *rc, int phase)
{
    (void)phase;
    priv_t *pr = rc->priv;
    warm_pages(rc, pr->t14_data);
}

/* ------------------------------------------------------------------ */
/* pattern 注册表                                                       */
/* ------------------------------------------------------------------ */

static int t3_setup(run_ctx *rc) { return stream_setup(rc, 3); }
static int t4_setup(run_ctx *rc) { return stream_setup(rc, 4); }
static int t5_setup(run_ctx *rc) { return stream_setup(rc, 5); }
static int t6_setup(run_ctx *rc) { return stream_setup(rc, 6); }
static int t15_setup(run_ctx *rc) { return stream_setup(rc, 15); }

static void t3_kernel(run_ctx *rc, int m, int t, int n) { stream_kernel(rc, m, t, n, 3); }
static void t4_kernel(run_ctx *rc, int m, int t, int n) { stream_kernel(rc, m, t, n, 4); }
static void t5_kernel(run_ctx *rc, int m, int t, int n) { stream_kernel(rc, m, t, n, 5); }
static void t6_kernel(run_ctx *rc, int m, int t, int n) { stream_kernel(rc, m, t, n, 6); }
static void t15_kernel(run_ctx *rc, int m, int t, int n) { stream_kernel(rc, m, t, n, 15); }

/* 统一收尾：释放 pattern 私有缓冲 + priv + 状态数组 */
static void fini_common(run_ctx *rc)
{
    priv_t *pr = rc->priv;
    if (pr) {
        switch (rc->pattern_id) {
        case 1:  free(pr->t1_b0); break;
        case 8:  free(pr->t8_idxmap); free(pr->t8_next); free(pr->t8_b0); break;
        case 9:  free(pr->t9_cdf); break;
        case 13: free(pr->t13_b0); break;
        case 14: free(pr->t14_list); free(pr->t14_b0); break;
        default: break;
        }
        free(pr);
        rc->priv = NULL;
    }
    bench_free_state(rc);
}

static void stream_fini(run_ctx *rc)
{
    priv_t *pr = rc->priv;
    if (pr) {
        free(pr->st_b0a);
        free(pr->st_b0b);
        free(pr->st_b0c);
    }
    fini_common(rc);
}

const pattern_ops g_patterns[] = {
    { "T1_hotwindow",   1,  t1_setup, reset_phase_generic, t1_warm, t1_kernel, expected_generic, fini_common,     0 },
    { "T2_randfull",    2,  t2_setup, reset_phase_generic, t2_warm, t2_kernel, expected_generic, fini_common,     0 },
    { "T3_copy",        3,  t3_setup, reset_phase_generic, stream_warm, t3_kernel, expected_generic, stream_fini,  0 },
    { "T4_scale",       4,  t4_setup, reset_phase_generic, stream_warm, t4_kernel, expected_generic, stream_fini,  0 },
    { "T5_add",         5,  t5_setup, reset_phase_generic, stream_warm, t5_kernel, expected_generic, stream_fini,  0 },
    { "T6_triad",       6,  t6_setup, reset_phase_generic, stream_warm, t6_kernel, expected_generic, stream_fini,  0 },
    { "T7_stride",      7,  t7_setup, reset_phase_generic, t7_warm, t7_kernel, expected_generic, fini_common,     1 },
    { "T8_pointerchase",8,  t8_setup, reset_phase_generic, warm_none, t8_kernel, expected_generic, fini_common,   64 },
    { "T9_zipf",        9,  t9_setup, reset_phase_generic, t9_warm, t9_kernel, expected_generic, fini_common,    64 },
    { "T10_workset",    10, t10_setup, reset_phase_generic, t10_warm, t10_kernel, expected_generic, fini_common,  0 },
    { "T11_firsttouch", 11, t11_setup, reset_phase_generic, t11_warm, t11_kernel, expected_generic, fini_common,  0 },
    { "T12_churn",      12, t12_setup, reset_phase_generic, t12_warm, t12_kernel, t12_expected, fini_common,      0 },
    { "T13_rmw",        13, t13_setup, reset_phase_generic, t13_warm, t13_kernel, expected_generic, fini_common,  0 },
    { "T14_gatherscatter", 14, t14_setup, reset_phase_generic, t14_warm, t14_kernel, expected_generic, fini_common, 0 },
    { "T15_hoist",      15, t15_setup, reset_phase_generic, stream_warm, t15_kernel, expected_generic, stream_fini, 0 },
};

#define NPATTERNS (int)(sizeof(g_patterns) / sizeof(g_patterns[0]))

int pattern_count(void) { return NPATTERNS; }
const pattern_ops *pattern_at(int i) { return &g_patterns[i]; }
const pattern_ops *pattern_by_id(int id)
{
    for (int i = 0; i < NPATTERNS; i++)
        if (g_patterns[i].id == id)
            return &g_patterns[i];
    return NULL;
}

void bench_free_state(run_ctx *rc)
{
    free(rc->va_of_idx); rc->va_of_idx = NULL;
    free(rc->touched);   rc->touched = NULL;
    free(rc->invalid);   rc->invalid = NULL;
    free(rc->cur_valid); rc->cur_valid = NULL;
}
