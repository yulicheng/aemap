#ifndef BENCH_PATTERNS_H
#define BENCH_PATTERNS_H

/* bench/patterns.h —— 地址序列生成器(test.md §3.5)
 *
 * 所有序列预生成(xorshift64*,固定种子),测量期间零 RNG 开销。
 * 约定:数据区 mmap 于 (1<<32) 起的 4GB 对齐段(idx→H 的"多对一"映射),
 * VVA = (idx<<32) | off,off 为段内偏移;direct 列表 = vva2va_pure(conv 列表)。
 */

#include <stdint.h>
#include <stddef.h>

struct pat_ctx {
    /* 输出:VVA 列表(conv)与真实 VA 列表(direct),二者解析后逐元素相等(§1.1) */
    uint64_t *conv, *direct;
    uint64_t *dst_conv, *dst_direct;    /* 仅 C8 拷贝目的流 */
    size_t     n;                       /* 列表长度 / 内核迭代数 */
    uint64_t   seed;

    /* 数据区(mmap 基址由调用方提供) */
    uint8_t   *data;
    size_t     data_size;               /* 生成后填写:数据区占用 */

    /* 参数 */
    uint32_t   K;                       /* 表索引范围 [0, K) */
    uint32_t   window;                  /* C2 窗口字节 */
    uint32_t   win_bytes;               /* C9 数据窗口 */
    uint32_t   stride;                  /* C7 idx 步长 */
    uint32_t   block_words;             /* C8 块内字数 */
    int        independent;             /* C4b:idx→独立数据页(数据足迹随 K) */

    /* C6 链表 */
    size_t    *chain_pos;               /* 节点访问次序(供 fill_chain 重填) */

    /* C14 基址(x/y 或 A/B/C 的 VVA) */
    uint64_t   extra_x, extra_y, extra_z;
};

/* 各生成器填写 conv 列表与 data_size;derive_direct 由映射完成后调用 */
void gen_seq(struct pat_ctx *c, uint64_t bytes);                /* C1:off 单调递增 */
void gen_hot(struct pat_ctx *c, size_t n, uint32_t window);     /* C2:窗口内反复 */
void gen_randoff(struct pat_ctx *c, size_t n, uint64_t span);   /* C3/C11/C12:idx 固定 off 随机 */
void gen_randidx(struct pat_ctx *c, size_t n);                  /* C4/C4b:idx 均匀随机 */
void gen_stride(struct pat_ctx *c, size_t n);                   /* C7:idx+stride,off 固定 */
void gen_zipf(struct pat_ctx *c, size_t n, double s);           /* C10:Zipf(s=0.9) */
void gen_gather(struct pat_ctx *c, size_t n);                   /* C9:idx+off 双随机稀疏 */
void gen_copy(struct pat_ctx *c, uint64_t bytes, int word_mode);/* C8:块级/字级双流 */
void gen_chase(struct pat_ctx *c, size_t hops);                 /* C6:链表节点次序 */
void gen_daxpy(struct pat_ctx *c, size_t n);                    /* C14a:双流基址 */
void gen_matmul(struct pat_ctx *c, uint32_t N);                 /* C14b:三流基址 */

void derive_direct(struct pat_ctx *c);   /* direct[i] = vva2va_pure(conv[i])(映射完成后) */
void fill_chain(struct pat_ctx *c, int conv_mode);  /* 链值按模式重填(VVA 或 VA) */
void chain_free(struct pat_ctx *c);

#endif /* BENCH_PATTERNS_H */
