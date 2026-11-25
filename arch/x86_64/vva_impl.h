#ifndef VVA_IMPL_X86_64_H
#define VVA_IMPL_X86_64_H

/* arch/x86_64/vva_impl.h —— B8 编码 + x86 最优序列(design.md §3)
 *
 * 构建变体(§5.4,由 Makefile 注入宏):
 *   ENC:  VVA_ENC_B8(默认,表项 = H<<32 预移位) / VVA_ENC_A4(C13 互换变体)
 *   TAG:  strict(默认,tag 任意) / fast(VVA_TAG_FAST,tag≡0 契约,省 1 条)
 *   BASE: pinned(-ffixed-r15 + 字面 %r15,默认;仅 GCC,附录 A)
 *         mat(基址作为操作数,lea 被 LICM 外提,循环内摊薄为 0;已实测)
 *         abs(-fno-pie -no-pie -mcmodel=small,SIB base=101 的 disp32 绝对形式)
 *   ISA:  v1(默认) / v3(BMI1 bextr;gas 对 imm 形式生成 XOP 编码,Intel 不支持,
 *         故用寄存器形式,控制值作为常量操作数由编译器外提——已实测)
 *
 * 无进位合并引理(§1.3):(H<<32) | off ≡ (H<<32) + off,表项低 32 位全零。
 */

#include "vva_common.h"

#if defined(VVA_ENC_B8)
extern uint64_t vva_map8[VVA_MAP_SIZE] __attribute__((aligned(VVA_TABLE_ALIGN))); /* 表项 = H<<32 */
#define VVA_TBL_SYM "vva_map8"
#define VVA_TBL_REF vva_map8
#elif defined(VVA_ENC_A4)
extern uint32_t vva_map4[VVA_MAP_SIZE] __attribute__((aligned(VVA_TABLE_ALIGN))); /* 表项 = H */
#define VVA_TBL_SYM "vva_map4"
#define VVA_TBL_REF vva_map4
#else
#error "VVA_ENC_B8 或 VVA_ENC_A4 必须二选一"
#endif

/* ---- 纯 C 参考(design §6):C0 golden 对照 + C13 编译器对照 ----
 * 非 volatile、无 "memory" clobber:允许编译器对相同输入 CSE,索引必须掩码 tag。 */
#if defined(VVA_ENC_B8)
static inline void *vva2va_pure(uint64_t vva) {
    return (void *)(vva_map8[(vva >> 32) & 0xFFFFFu] | (uint32_t)vva);
}
#else
static inline void *vva2va_pure(uint64_t vva) {
    return (void *)(((uint64_t)vva_map4[(vva >> 32) & 0xFFFFFu] << 32) | (uint32_t)vva);
}
#endif

/* ---- 段缓存变体(design §6 `cache`,SEG=1 构建):idx==last 跳过查表(分支预测型) ---- */
#if defined(VVA_SEG_CACHE)
static inline void *vva2va(uint64_t vva) {
    static uint32_t s_idx = 0xFFFFFFFFu;
    static uint64_t s_hi;
    uint32_t idx = (uint32_t)((vva >> 32) & 0xFFFFFu);
    uint64_t hi;
    if (__builtin_expect(idx == s_idx, 1)) {
        hi = s_hi;
    } else {
#if defined(VVA_ENC_B8)
        hi = vva_map8[idx];
#else
        hi = (uint64_t)vva_map4[idx] << 32;
#endif
        s_idx = idx;
        s_hi = hi;
    }
    return (void *)(hi | (uint32_t)vva);
}
#define VVA_CONV_INSN 0   /* 分支型:条数随命中率变化,跳过硬校验 */

#elif defined(VVA_VARIANT_PURE)
/* ---- 纯 C 变体(C13 对照):交给编译器生成,验证手写序列是否更优 ---- */
static inline void *vva2va(uint64_t vva) { return vva2va_pure(vva); }
#define VVA_CONV_INSN 0   /* 编译器生成,条数由实测为准 */

#else
/* ---- 内联汇编最优序列(design §3.2/§3.3) ---- */

#if defined(VVA_TAG_FAST)
#define VVA_TAG_CONTRACT_ZERO 1   /* fast 契约标记(test.md C0-3 验证其存在) */
#endif

/* ISA v3:需要 -march=x86-64-v3(定义 __BMI__;BMI1 的标准宏名,clang 同名)。
 * 控制字编码:start 在低 8 位、len 在高 8 位 → start=32,len=20 → (20<<8)|32 = 0x1420
 * (与 _bextr_u64(v,32,20) 生成的控制字一致,已实测)。
 * gas 对 bextr 立即数形式生成 XOP 编码(Intel 不支持),故固定寄存器形式。 */
#if defined(VVA_ISA_V3) && defined(__BMI__)
#define VVA_X86_BEXTR 1
#endif

/* 输入操作数列表按 BASE / BEXTR 组合展开 */
#if defined(VVA_BASE_PINNED) || defined(VVA_BASE_ABS)
# if defined(VVA_X86_BEXTR) && !defined(VVA_TAG_FAST)
#  define VVA_ASM_IN : [ctl] "r"(ctl)
# else
#  define VVA_ASM_IN :
# endif
#else
# if defined(VVA_X86_BEXTR) && !defined(VVA_TAG_FAST)
#  define VVA_ASM_IN : [tbl] "r"(VVA_TBL_REF), [ctl] "r"(ctl)
# else
#  define VVA_ASM_IN : [tbl] "r"(VVA_TBL_REF)
# endif
#endif

static inline void *vva2va(uint64_t vva) {
    uint64_t off, va;
#if defined(VVA_X86_BEXTR) && !defined(VVA_TAG_FAST)
    /* 控制值 = (len<<8)|start;作为常量操作数,编译器将其 mov 外提到循环外(已实测) */
    const uint64_t ctl = (20UL << 8) | 32UL;
#endif

    __asm__ volatile (
        "movl  %k[in], %k[off]                       \n\t"  /* off = VVA[31:0](mov 消除,0 延迟) */
#if defined(VVA_TAG_FAST)
        "shr   $32, %[in]                            \n\t"  /* fast:tag≡0 契约,shr 后即干净 idx */
#elif defined(VVA_X86_BEXTR)
        "bextr %[ctl], %[in], %[in]                  \n\t"  /* idx = VVA[51:32],bextr 天然忽略 tag */
#else
        "shr   $32, %[in]                            \n\t"  /* in = VVA[63:32] */
        "and   $0xFFFFF, %k[in]                      \n\t"  /* idx,清除 tag 残留(§3.2 关键) */
#endif
#if defined(VVA_ENC_B8)
# if defined(VVA_BASE_PINNED)
        "movq  (%%r15,%[in],8), %[va]                \n\t"  /* 字面引用 r15(需 -ffixed-r15) */
# elif defined(VVA_BASE_ABS)
        "movq  " VVA_TBL_SYM "(,%[in],8), %[va]      \n\t"  /* disp32 绝对,需 -fno-pie -mcmodel=small */
# else
        "movq  (%[tbl],%[in],8), %[va]               \n\t"  /* 基址操作数;lea 可被外提(已实测) */
# endif
#else /* A4(C13 互换):表项为 32 位 H,需 shl 合并 */
# if defined(VVA_BASE_PINNED)
        "movl  (%%r15,%[in],4), %k[va]               \n\t"  /* 32 位加载零扩展(§4.4 前提) */
# elif defined(VVA_BASE_ABS)
        "movl  " VVA_TBL_SYM "(,%[in],4), %k[va]     \n\t"
# else
        "movl  (%[tbl],%[in],4), %k[va]              \n\t"
# endif
        "shlq  $32, %[va]                            \n\t"  /* H<<32(+1 条,§8.1 编码互换代价) */
#endif
        "orq   %[off], %[va]                         \n\t"  /* 无进位合并(§1.3),VA[63:52] 恒 0 */
        : [va] "=&r"(va), [off] "=&r"(off), [in] "+r"(vva)
        VVA_ASM_IN
        : "memory");
    return (void *)va;
}

/* 循环上下文每访问转换指令数(design §8.1;mat/abs 的基址物化在循环内摊薄为 0) */
#if defined(VVA_ENC_B8)
# if defined(VVA_TAG_FAST)
#  define VVA_CONV_INSN 4
# elif defined(VVA_X86_BEXTR)
#  define VVA_CONV_INSN 4
# else
#  define VVA_CONV_INSN 5
# endif
#else
# if defined(VVA_X86_BEXTR)
#  define VVA_CONV_INSN 5
# elif defined(VVA_TAG_FAST)
#  define VVA_CONV_INSN 5
# else
#  define VVA_CONV_INSN 6
# endif
#endif

#endif /* VVA_SEG_CACHE / PURE */

#endif /* VVA_IMPL_X86_64_H */
