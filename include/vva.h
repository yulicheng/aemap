#ifndef VVA_H
#define VVA_H

/* vva.h —— 公共宏接口(design.md §5.2)
 *
 * 派发仅在头文件包含层发生:内核与框架代码 #include "vva.h" 即可,
 * 不含任何架构 #if 分支。各架构实现头(arch/x86_64 与 arch/aarch64)提供:
 *   static inline void *vva2va(uint64_t vva);      最优指令序列(ENC/TAG/BASE/ISA 变体)
 *   static inline void *vva2va_pure(uint64_t vva); 纯 C 参考(golden 对照,C13 编译器对照)
 *   #define VVA_CONV_INSN <n>                      循环上下文每访问转换指令数(design §8.1)
 *   #define VVA_TAG_CONTRACT_ZERO 1                 仅 fast 构建:tag≡0 契约标记(C0-3)
 * 表与映射 API(src/vva_map.c):
 *   vva_map_init / vva_map_set / vva_table_base / vva_table_entry_bytes
 */

#include <stdint.h>
#include <stddef.h>
#include "vva_common.h"

#if defined(__x86_64__)
  #include "arch/x86_64/vva_impl.h"
#elif defined(__aarch64__)
  #include "arch/aarch64/vva_impl.h"
#else
  #error unsupported arch
#endif

/* ---- VVA 构造/分解(design §1.1):idx=[51:32] off=[31:0] tag=[63:52] 忽略 ---- */
#define VVA_MAKE(idx, off)  ((((uint64_t)(idx) & 0xFFFFFu) << 32) | ((uint32_t)(off)))
#define VVA_IDX_OF(vva)     ((uint32_t)(((uint64_t)(vva) >> 32) & 0xFFFFFu))
#define VVA_OFF_OF(vva)     ((uint32_t)(uint64_t)(vva))
#define VVA_TAG_OF(vva)     ((uint16_t)((uint64_t)(vva) >> 52))

/* ---- 映射表 API(src/vva_map.c,唯一写入口 design §5.3/§7.4) ---- */
void   vva_map_init(void);                 /* 全表清零(entry0 = 未映射哨兵 §1.5) */
void   vva_map_set(uint32_t idx, uint32_t H);  /* 断言 idx/越界 H(§1.4) */
void  *vva_table_base(void);               /* 当前编码的活动表基址 */
size_t vva_table_entry_bytes(void);        /* A4:4,B8:8 */
void   vva_table_enable_hugepage(void);    /* 可选:HP 构建下把表注册为 hugepage(C12) */
void   vva_tbl_pin(void);                  /* pinned 构建:main 入口显式调用(见 src/vva_map.c 注) */

/* ---- 可选数据预取(design §5.2):x86 prefetcht0 / ARM prfm ---- */
static inline void vva_prefetch(uint64_t vva) {
    __builtin_prefetch(vva2va(vva), 0, 3);
}

#define VVA_PTR(p)          ((void *)(uintptr_t)vva2va((uint64_t)(p)))
#define VVA_LD(type, p)     (*(type *)(uintptr_t)VVA_PTR(p))
#define VVA_ST(type, p, v)  (VVA_LD(type, p) = (v))

#endif /* VVA_H */
