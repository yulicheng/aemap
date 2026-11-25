/* bench/kernel_defs.h —— 内核定义(test.md §3.8)
 *
 * ⚠ 内部头:被 kernels.c 以 CONV=0(direct)与 CONV=1(conv)各包含一次。
 * 内核只写一遍,唯一差异层是 A2P 宏:
 *   direct: A2P(a) ≡ (void*)(uintptr_t)(a)   —— 列表内已是真实 VA
 *   conv:   A2P(a) ≡ vva2va(a)               —— 每次 ld/st 内联转换
 * 两实例指令流逐指令对应,Δins/op 与 design.md §8.1 硬校验。
 */
#if !defined(CONV)
#error "kernel_defs.h 由 kernels.c 以 CONV=0/1 包含"
#endif

#if CONV
#define A2P(a)      vva2va((uint64_t)(a))
#define KGLUE2_(a,b) a##b
#define KGLUE2(a,b)  KGLUE2_(a,b)
#define KNAME(n)     KGLUE2(n, _c)
#else
#define A2P(a)      ((void *)(uintptr_t)(a))
#define KGLUE2_(a,b) a##b
#define KGLUE2(a,b)  KGLUE2_(a,b)
#define KNAME(n)     KGLUE2(n, _d)
#endif

extern volatile uint64_t g_sink;
extern void (*volatile g_hop_hook)(void);   /* C6 冷表:每跳 flush 表行(两模式对称) */

/* 序列基址屏障(§3.7):阻断编译器对指针来源的假设(如指向已知数据 → 提升为常量) */
#define KBASE(var) __asm__ volatile("" : "+r"(var))

/* ---------------- 读流 ---------------- */
static void KNAME(k_read)(const uint64_t *addr, size_t n, uint64_t p1, uint64_t p2) {
    (void)p1; (void)p2;
    uint32_t acc = 0;
    KBASE(addr);
    for (size_t i = 0; i < n; i++)
        acc ^= *(const uint32_t *)A2P(addr[i]);
    g_sink += acc;
}

/* ---------------- 写流 ---------------- */
static void KNAME(k_write)(const uint64_t *addr, size_t n, uint64_t p1, uint64_t p2) {
    (void)p1; (void)p2;
    uint32_t acc = 0;
    KBASE(addr);
    for (size_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)i ^ acc;
        *(uint32_t *)A2P(addr[i]) = v;
        acc = acc * 31 + v;
    }
    g_sink += acc;
}

/* ---------------- 读改写(C11) ---------------- */
static void KNAME(k_rmw)(const uint64_t *addr, size_t n, uint64_t p1, uint64_t p2) {
    (void)p1; (void)p2;
    uint32_t acc = 0;
    KBASE(addr);
    for (size_t i = 0; i < n; i++) {
        uint32_t v = *(uint32_t *)A2P(addr[i]);
        v ^= 1u;
        *(uint32_t *)A2P(addr[i]) = v;
        acc ^= v;
    }
    g_sink += acc;
}

/* ---------------- 块级转换拷贝(C8):每 64/128B 转换一次指针 ---------------- */
static void KNAME(k_copy_blk)(const uint64_t *addr, size_t n, uint64_t p1, uint64_t p2) {
    const uint64_t *dst = (const uint64_t *)(uintptr_t)p1;
    size_t w = (size_t)p2 ? (size_t)p2 : 16;   /* 每块字数 */
    uint64_t acc = 0;
    KBASE(addr); KBASE(dst);
    for (size_t b = 0; b < n; b++) {
        const uint32_t *s = (const uint32_t *)A2P(addr[b]);
        uint32_t *d = (uint32_t *)A2P(dst[b]);
        for (size_t i = 0; i < w; i++)
            d[i] = s[i];
        acc ^= (uint64_t)(uintptr_t)s ^ (uint64_t)(uintptr_t)d;
    }
    g_sink += acc;
}

/* ---------------- 字级转换拷贝(C8 上界变体):每字一次转换 ---------------- */
static void KNAME(k_copy_word)(const uint64_t *addr, size_t n, uint64_t p1, uint64_t p2) {
    const uint64_t *dst = (const uint64_t *)(uintptr_t)p1;
    uint32_t acc = 0;
    (void)p2;
    KBASE(addr); KBASE(dst);
    for (size_t i = 0; i < n; i++) {
        uint32_t v = *(const uint32_t *)A2P(addr[i]);
        *(uint32_t *)A2P(dst[i]) = v;
        acc ^= v;
    }
    g_sink += acc;
}

/* ---------------- 非临时写拷贝(C8 NT 变体;x86 流式写验证转换与写路径重叠) ---------------- */
static void KNAME(k_copy_nt)(const uint64_t *addr, size_t n, uint64_t p1, uint64_t p2) {
    const uint64_t *dst = (const uint64_t *)(uintptr_t)p1;
    size_t w = (size_t)p2 ? (size_t)p2 : 16;
    uint64_t acc = 0;
    KBASE(addr); KBASE(dst);
    for (size_t b = 0; b < n; b++) {
        const uint32_t *s = (const uint32_t *)A2P(addr[b]);
        uint32_t *d = (uint32_t *)A2P(dst[b]);
#if defined(__x86_64__)
        for (size_t i = 0; i < w; i++)
            _mm_stream_si32((int *)(d + i), (int)s[i]);
        _mm_sfence();
#else
        for (size_t i = 0; i < w; i++)
            d[i] = s[i];   /* ARMv8.0 基线无用户态 NT store,退化为普通写 */
#endif
        acc ^= (uint64_t)(uintptr_t)s ^ (uint64_t)(uintptr_t)d;
    }
    g_sink += acc;
}

/* ---------------- 指针追逐(C6):链值存于数据区,依赖加载 ---------------- */
static void KNAME(k_chase)(const uint64_t *addr, size_t n, uint64_t p1, uint64_t p2) {
    (void)p1; (void)p2;
    uint64_t p = addr[0];
    KBASE(p);
    for (size_t i = 0; i < n; i++) {
        p = *(const uint64_t *)A2P(p);
        if (g_hop_hook)
            g_hop_hook();     /* 冷表:每跳 flush 表行;direct 侧同样执行,成本对称 */
    }
    g_sink += p;
}

/* ---------------- daxpy(C14a):y[i] = a*x[i] + y[i],2 条流 ---------------- */
static void KNAME(k_daxpy)(const uint64_t *addr, size_t n, uint64_t p1, uint64_t p2) {
    uint64_t xb = addr[0], yb = p1;
    double a;
    memcpy(&a, &p2, sizeof(a));
    KBASE(xb); KBASE(yb);
    for (size_t i = 0; i < n; i++)
        *(double *)A2P(yb + 8 * i) += a * *(const double *)A2P(xb + 8 * i);
    g_sink += (uint64_t)(uintptr_t)yb;
}

/* ---------------- matmul 内层(C14b):C[i][j] += A[i][k]*B[k][j],3 条流 ---------------- */
static void KNAME(k_matmul)(const uint64_t *addr, size_t n, uint64_t p1, uint64_t p2) {
    size_t N = n;
    uint64_t A = addr[0], B = p1, C = p2;
    KBASE(A); KBASE(B); KBASE(C);
    for (size_t i = 0; i < N; i++)
        for (size_t j = 0; j < N; j++) {
            double s = 0;
            for (size_t k = 0; k < N; k++)
                s += *(const double *)A2P(A + 8 * (i * N + k))
                   * *(const double *)A2P(B + 8 * (k * N + j));
            *(double *)A2P(C + 8 * (i * N + j)) += s;
        }
    g_sink += 1;
}

#undef A2P
#undef KNAME
#undef KGLUE2
#undef KGLUE2_
#undef KBASE
