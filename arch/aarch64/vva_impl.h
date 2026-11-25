#ifndef VVA_IMPL_AARCH64_H
#define VVA_IMPL_AARCH64_H

/* arch/aarch64/vva_impl.h —— A4 编码 + ARM 最优序列(design.md §4)
 *
 * 构建变体(§5.4,由 Makefile 注入宏):
 *   ENC:  VVA_ENC_A4(默认,表项 = H) / VVA_ENC_B8(C13 互换变体,表项 = H<<32)
 *   TAG:  strict(默认,tag 任意) / fast(VVA_TAG_FAST,tag≡0 契约)
 *   BASE: pinned(-ffixed-x27 + 字面 x27,默认;GCC/clang 均支持,附录 A)
 *         mat(基址作为操作数,adrp+add 在循环内摊薄为 0)
 *
 * 所有指令均为 ARMv8.0 基线,无需额外 ISA 扩展(LVA 仅影响 W 上限,§1.4)。
 * strict 用 orr 版:第二条 ubfx 与 ldr 并行发射,对乱序核更稳(§4.4)。
 * 无进位合并引理(§1.3):表项高位补零 + ldr w 零扩展是 orr/bfi 正确性的前提。
 */

#include "vva_common.h"

#if defined(VVA_ENC_A4)
extern uint32_t vva_map4[VVA_MAP_SIZE] __attribute__((aligned(VVA_TABLE_ALIGN))); /* 表项 = H */
#define VVA_TBL_REF vva_map4
#elif defined(VVA_ENC_B8)
extern uint64_t vva_map8[VVA_MAP_SIZE] __attribute__((aligned(VVA_TABLE_ALIGN))); /* 表项 = H<<32 */
#define VVA_TBL_REF vva_map8
#else
#error "VVA_ENC_A4 或 VVA_ENC_B8 必须二选一"
#endif

/* ---- 纯 C 参考(design §6):C0 golden 对照 + C13 编译器对照 ---- */
#if defined(VVA_ENC_A4)
static inline void *vva2va_pure(uint64_t vva) {
    return (void *)(((uint64_t)vva_map4[(vva >> 32) & 0xFFFFFu] << 32) | (uint32_t)vva);
}
#else
static inline void *vva2va_pure(uint64_t vva) {
    return (void *)(vva_map8[(vva >> 32) & 0xFFFFFu] | (uint32_t)vva);
}
#endif

/* ---- 段缓存变体(design §6 `cache`,SEG=1 构建) ---- */
#if defined(VVA_SEG_CACHE)
static inline void *vva2va(uint64_t vva) {
    static uint32_t s_idx = 0xFFFFFFFFu;
    static uint64_t s_hi;
    uint32_t idx = (uint32_t)((vva >> 32) & 0xFFFFFu);
    uint64_t hi;
    if (__builtin_expect(idx == s_idx, 1)) {
        hi = s_hi;
    } else {
#if defined(VVA_ENC_A4)
        hi = (uint64_t)vva_map4[idx] << 32;
#else
        hi = vva_map8[idx];
#endif
        s_idx = idx;
        s_hi = hi;
    }
    return (void *)(hi | (uint32_t)vva);
}
#define VVA_CONV_INSN 0   /* 分支型,跳过硬校验 */

#elif defined(VVA_VARIANT_PURE)
/* ---- 纯 C 变体(C13 对照) ---- */
static inline void *vva2va(uint64_t vva) { return vva2va_pure(vva); }
#define VVA_CONV_INSN 0

#else
/* ---- 内联汇编最优序列(design §4.2/§4.3) ---- */

#if defined(VVA_TAG_FAST)
#define VVA_TAG_CONTRACT_ZERO 1   /* fast 契约标记(test.md C0-3 验证其存在) */
#endif

static inline void *vva2va(uint64_t vva) {
    uint64_t va, idx, base, off;

    __asm__ volatile (
        "ubfx  %[idx], %[vva], #32, #20              \n\t"  /* idx = VVA[51:32],UBFX 天然忽略 tag */
#if defined(VVA_ENC_A4)
# if defined(VVA_BASE_PINNED)
        "ldr   %w[base], [x27, %[idx], lsl #2]       \n\t"  /* 字面引用 x27(需 -ffixed-x27) */
# else
        "ldr   %w[base], [%[tbl], %[idx], lsl #2]    \n\t"  /* 32 位加载零扩展 */
# endif
#else /* B8(C13 互换):表项 = H<<32 预移位,64 位加载 */
# if defined(VVA_BASE_PINNED)
        "ldr   %[base], [x27, %[idx], lsl #3]        \n\t"
# else
        "ldr   %[base], [%[tbl], %[idx], lsl #3]     \n\t"
# endif
#endif
#if defined(VVA_TAG_FAST)
# if defined(VVA_ENC_A4)
        "bfi   %[vva], %[base], #32, #20             \n\t"  /* 保留 [31:0] 与 [63:52](tag≡0 契约) */
# else
        "bfxil %[base], %[vva], #0, #32              \n\t"  /* base[31:0]=off,base[63:32]=H<<32 */
# endif
#else
        "ubfx  %[off], %[vva], #0, #32               \n\t"  /* off = VVA[31:0],与 ldr 并行发射 */
# if defined(VVA_ENC_A4)
        "orr   %[va], %[off], %[base], lsl #32       \n\t"  /* VA = off | (H<<32),[63:52] 恒 0 */
# else
        "orr   %[va], %[base], %[off]                \n\t"
# endif
#endif
        : [va] "=&r"(va), [idx] "=&r"(idx), [base] "=&r"(base), [off] "=&r"(off), [vva] "+r"(vva)
#if defined(VVA_BASE_PINNED)
        :
#else
        : [tbl] "r"(VVA_TBL_REF)
#endif
        : "memory");

#if defined(VVA_TAG_FAST) && defined(VVA_ENC_A4)
    return (void *)vva;        /* bfi 直接在 vva 上合并 */
#elif defined(VVA_TAG_FAST)
    return (void *)base;       /* B8 fast:结果落在 base */
#else
    return (void *)va;
#endif
}

/* 循环上下文每访问转换指令数(design §8.1;mat 的 adrp+add 在循环内摊薄为 0) */
#if defined(VVA_TAG_FAST)
#define VVA_CONV_INSN 3
#else
#define VVA_CONV_INSN 4
#endif

#endif /* VVA_SEG_CACHE / PURE */

#endif /* VVA_IMPL_AARCH64_H */
