#ifndef VVA_COMMON_H
#define VVA_COMMON_H

/* 映射参数 —— design.md §1.2/§1.4/§1.6
 * 裁定(2026-11): 真实 VA 为 48 位,readme 的 "VA 高 48 到 32 位" 按 [47:32] 理解,
 * 默认 W = VVA_HIGH_BITS = 16;x86-64 4 级页表用户态 bit47 恒 0,有效上限 15(VVA_W_MAX)。
 * 代码序列与 W 无关(表项高位补零),W 只影响表内容合法性与 vva_map_set 断言。 */

#define VVA_IDX_BITS 20
#define VVA_MAP_SIZE (1u << VVA_IDX_BITS)   /* 2^20 = 1M 项 */

#ifndef VVA_HIGH_BITS
#define VVA_HIGH_BITS 16
#endif

#ifndef VVA_W_MAX
#if defined(__x86_64__)
#define VVA_W_MAX 15   /* 4 级页表用户态;LA57 构建可 -DVVA_W_MAX=20 */
#elif defined(__aarch64__)
#define VVA_W_MAX 16   /* 48 位 VA;ARMv8.2-LVA 构建可 -DVVA_W_MAX=20 */
#endif
#endif

#if VVA_HIGH_BITS > 20
#error "VVA_HIGH_BITS 最大 20(52 位新地址)"
#endif
#if !defined(VVA_W_MAX) || VVA_W_MAX > 20 || VVA_W_MAX == 0
#error "VVA_W_MAX 非法(1..20)"
#endif

/* 表项编码默认: x86_64 → B8(8B 预移位), aarch64 → A4(4B 原始);C13 可互换。
 * 取舍依据见 design.md §2.2。 */
#if !defined(VVA_ENC_A4) && !defined(VVA_ENC_B8)
#if defined(__x86_64__)
#define VVA_ENC_B8 1
#else
#define VVA_ENC_A4 1
#endif
#endif

#if defined(VVA_ENC_A4) && defined(VVA_ENC_B8)
#error "VVA_ENC_A4 与 VVA_ENC_B8 互斥"
#endif

/* 表对齐: VVA_TABLE_HP 构建 2MB 对齐,便于 hugepage madvise(test.md C12) */
#ifdef VVA_TABLE_HP
#define VVA_TABLE_ALIGN (2u << 20)
#else
#define VVA_TABLE_ALIGN 64
#endif

#endif /* VVA_COMMON_H */
