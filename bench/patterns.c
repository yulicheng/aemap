/* bench/patterns.c —— 地址序列生成器(test.md §3.5 / §4)
 *
 * 覆盖 readme 要求的两大场景(C2 相近 VVA→表 L1 命中;C4 随机索引→表 miss 阶梯)
 * 及常见软件访存 pattern:seq/hot/randoff/randidx/stride/zipf/gather/copy/chase/
 * daxpy/matmul。关键解耦技巧(§3.5):默认所有 idx → 同一 H,数据工作集恒定,
 * 表足迹随 K 增长;C4b 用 idx→独立数据页叠加数据 miss。
 */
#include "patterns.h"
#include "harness.h"
#include "vva.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define IDX_BASE 1u   /* 段 idx 默认值;映射为 H = 数据段高 32 位(见 main.c) */

static uint64_t rnd(struct pat_ctx *c) { return xs64(&c->seed); }

/* off 对齐到 4 字节(u32 访存);chase/浮点用 8 字节 */
static uint64_t align4(uint64_t v) { return v & ~(uint64_t)3; }
static uint64_t align8(uint64_t v) { return v & ~(uint64_t)7; }

void derive_direct(struct pat_ctx *c) {
    /* direct 列表由同一映射函数预计算(§3.5),保证两模式访问相同 cache line */
    for (size_t i = 0; i < c->n; i++)
        c->direct[i] = (uint64_t)(uintptr_t)vva2va_pure(c->conv[i]);
    if (c->dst_conv)
        for (size_t i = 0; i < c->n; i++)
            c->dst_direct[i] = (uint64_t)(uintptr_t)vva2va_pure(c->dst_conv[i]);
}

void chain_free(struct pat_ctx *c) {
    free(c->chain_pos);
    c->chain_pos = NULL;
}

/* ---------------- C1 顺序流 ---------------- */
void gen_seq(struct pat_ctx *c, uint64_t bytes) {
    c->n = bytes / 4;
    for (size_t i = 0; i < c->n; i++)
        c->conv[i] = VVA_MAKE(IDX_BASE, 4 * i);
    c->data_size = bytes;
}

/* ---------------- C2 相近 VVA 热点 ---------------- */
void gen_hot(struct pat_ctx *c, size_t n, uint32_t window) {
    size_t words = window / 4;
    if (!words) words = 1;
    for (size_t i = 0; i < n; i++)
        c->conv[i] = VVA_MAKE(IDX_BASE, (uint32_t)(4 * (i % words)));
    c->n = n;
    c->data_size = window < 4096 ? 4096 : window;
}

/* ---------------- C3/C11/C12 段内随机 off ---------------- */
void gen_randoff(struct pat_ctx *c, size_t n, uint64_t span) {
    if (!span) span = 4096;
    for (size_t i = 0; i < n; i++)
        c->conv[i] = VVA_MAKE(IDX_BASE, (uint32_t)align4(rnd(c) % span));
    c->n = n;
    c->data_size = span < 4096 ? 4096 : span;
}

/* ---------------- C4 随机索引 ---------------- */
void gen_randidx(struct pat_ctx *c, size_t n) {
    /* 默认多对一:idx 随机、off 固定在首 4KB 数据页 → 数据足迹恒定,
     * 表足迹 = K 项(§3.5);C4b(independent):每个 idx 一页,数据足迹 = K 页 */
    uint32_t K = c->K ? c->K : 1;
    for (size_t i = 0; i < n; i++) {
        uint32_t idx = (uint32_t)(rnd(c) % K);
        uint64_t off;
        if (c->independent)
            off = 4096ULL * idx + (rnd(c) & 0xFF8);      /* ≤ 4096*K < 2^32(K≤1M) */
        else
            off = rnd(c) & 0xFF8;
        c->conv[i] = VVA_MAKE(idx, (uint32_t)off);
    }
    c->n = n;
    c->data_size = c->independent ? 4096ULL * K : 4096;
}

/* ---------------- C7 跨段步长 ---------------- */
void gen_stride(struct pat_ctx *c, size_t n) {
    uint32_t K = c->K ? c->K : 1;
    uint32_t st = c->stride ? c->stride : 1;
    for (size_t i = 0; i < n; i++) {
        uint32_t idx = (uint32_t)((i * (uint64_t)st) % K);
        c->conv[i] = VVA_MAKE(idx, 0x40);                /* off 固定,表沿线顺序流 */
    }
    c->n = n;
    c->data_size = 4096;
}

/* ---------------- C10 Zipf ---------------- */
void gen_zipf(struct pat_ctx *c, size_t n, double s) {
    uint32_t K = c->K ? c->K : 1;
    double *cdf = malloc(K * sizeof(double));
    double sum = 0;
    for (uint32_t i = 0; i < K; i++) {
        sum += 1.0 / pow((double)i + 1.0, s);
        cdf[i] = sum;
    }
    for (uint32_t i = 0; i < K; i++) cdf[i] /= sum;
    for (size_t i = 0; i < n; i++) {
        double u = (double)(rnd(c) >> 11) * (1.0 / 9007199254740992.0);
        /* 二分找 idx:cdf[idx] >= u */
        uint32_t lo = 0, hi = K - 1;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (cdf[mid] < u) lo = mid + 1; else hi = mid;
        }
        c->conv[i] = VVA_MAKE(lo, (uint32_t)(rnd(c) & 0xFF8));
    }
    free(cdf);
    c->n = n;
    c->data_size = 4096;
}

/* ---------------- C9 gather/scatter ---------------- */
void gen_gather(struct pat_ctx *c, size_t n) {
    uint32_t K = c->K ? c->K : 1;
    uint64_t win = c->win_bytes ? c->win_bytes : 65536;
    for (size_t i = 0; i < n; i++)
        c->conv[i] = VVA_MAKE((uint32_t)(rnd(c) % K),
                              (uint32_t)align4(rnd(c) % win));
    c->n = n;
    c->data_size = win < 4096 ? 4096 : win;
}

/* ---------------- C8 拷贝 ---------------- */
void gen_copy(struct pat_ctx *c, uint64_t bytes, int word_mode) {
    /* 布局契约(main.c 依赖):源列表 conv[0..n),目的列表自 conv[n+1] 起,
     * cap = 2n+2;derive_direct 按 n 填 direct 双流。 */
    if (word_mode) {
        c->n = bytes / 4;
        c->block_words = 1;
        c->dst_conv = &c->conv[c->n + 1];
        for (size_t i = 0; i < c->n; i++) {
            c->conv[i]     = VVA_MAKE(IDX_BASE, (uint32_t)(4 * i));
            c->dst_conv[i] = VVA_MAKE(IDX_BASE, (uint32_t)(bytes + 4 * i));
        }
    } else {
        uint32_t bw = c->block_words ? c->block_words : 16;   /* 64B/块 */
        uint64_t bb = (uint64_t)bw * 4;
        c->n = bytes / bb;
        c->dst_conv = &c->conv[c->n + 1];
        for (size_t b = 0; b < c->n; b++) {
            c->conv[b]     = VVA_MAKE(IDX_BASE, (uint32_t)(b * bb));
            c->dst_conv[b] = VVA_MAKE(IDX_BASE, (uint32_t)(bytes + b * bb));
        }
    }
    c->data_size = 2 * bytes;
}

/* ---------------- C6 指针追逐 ---------------- */
void gen_chase(struct pat_ctx *c, size_t hops) {
    /* 节点 = 数据区 8 字节槽;随机置换得到链序(环形),链值按模式在运行前重填 */
    c->n = hops;
    c->data_size = hops * 8;
    c->chain_pos = malloc(hops * sizeof(size_t));
    for (size_t i = 0; i < hops; i++) c->chain_pos[i] = i;
    for (size_t i = hops - 1; i > 0; i--) {          /* Fisher-Yates */
        size_t j = rnd(c) % (i + 1);
        size_t t = c->chain_pos[i]; c->chain_pos[i] = c->chain_pos[j]; c->chain_pos[j] = t;
    }
    fill_chain(c, 1);                                /* 初始按 conv 填(映射前 harmless) */
}

void fill_chain(struct pat_ctx *c, int conv_mode) {
    if (!c->chain_pos || !c->data) return;
    volatile uint64_t *nodes = (volatile uint64_t *)c->data;
    size_t hops = c->n;
    for (size_t i = 0; i < hops; i++) {
        uint64_t next = VVA_MAKE(IDX_BASE, (uint32_t)(8 * c->chain_pos[(i + 1) % hops]));
        if (conv_mode)
            nodes[c->chain_pos[i]] = next;
        else
            nodes[c->chain_pos[i]] = (uint64_t)(uintptr_t)vva2va_pure(next);
    }
    c->conv[0] = VVA_MAKE(IDX_BASE, (uint32_t)(8 * c->chain_pos[1 % hops]));
    c->direct[0] = (uint64_t)(uintptr_t)vva2va_pure(c->conv[0]);
}

/* ---------------- C14a daxpy ---------------- */
void gen_daxpy(struct pat_ctx *c, size_t n) {
    c->n = n;
    c->extra_x = VVA_MAKE(IDX_BASE, 0);              /* x = [0, 8n) */
    c->extra_y = VVA_MAKE(IDX_BASE, align8(8 * n));  /* y = [8n, 16n) */
    c->data_size = 16 * n;
}

/* ---------------- C14b matmul ---------------- */
void gen_matmul(struct pat_ctx *c, uint32_t N) {
    c->n = N;
    uint64_t a = 8ULL * N * N;
    c->extra_x = VVA_MAKE(IDX_BASE, 0);              /* A */
    c->extra_y = VVA_MAKE(IDX_BASE, (uint32_t)a);    /* B(8N² 后) */
    c->extra_z = VVA_MAKE(IDX_BASE, (uint32_t)(2 * a)); /* C(16N² 后) */
    c->data_size = 3 * a + 64;
}
