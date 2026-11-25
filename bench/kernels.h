#ifndef BENCH_KERNELS_H
#define BENCH_KERNELS_H

/* bench/kernels.h —— 内核派发(内核与架构无关,test.md §2) */

#include <stdint.h>
#include <stddef.h>

/* 内核统一签名:addr = VVA/VA 列表或基址,n = 迭代数,p1/p2 = 变体参数。
 * 结果写入 g_sink 防 DCE(test.md §3.7)。 */
typedef void (*kern_fn)(const uint64_t *addr, size_t n, uint64_t p1, uint64_t p2);

enum kern_id {
    K_READ,       /* u32 读流:xor 累积 —— C1r/C2/C3/C4/C5/C7/C9/C10/C12/C15/C16 */
    K_WRITE,      /* u32 写流 —— C1w/C11w/C15(contend 争用) */
    K_RMW,        /* u32 读改写 —— C11rmw */
    K_COPY_BLK,   /* 块级转换拷贝(每 64/128B 转换一次)—— C8 */
    K_COPY_WORD,  /* 字级转换拷贝(每字转换,上界变体)—— C8w/C15(STREAM Copy) */
    K_COPY_NT,    /* 非临时写拷贝(x86 NT store 变体)—— C8nt */
    K_CHASE,      /* 依赖加载链 —— C6 */
    K_DAXPY,      /* y = a*x + y(STREAM Triad)—— C14a/C15 */
    K_SCALE,      /* a = s*b(STREAM Scale)—— C15 */
    K_ADD,        /* c = a+b(STREAM Add)—— C15 */
    K_MATMUL,     /* C[i][j] += A[i][k]*B[k][j] —— C14b */
    K_N
};

kern_fn kern_get(enum kern_id id, int conv);   /* conv=1 返回转换实例 */

#endif /* BENCH_KERNELS_H */
