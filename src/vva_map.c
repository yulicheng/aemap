/* src/vva_map.c —— 映射表定义与唯一写入口(design.md §5.3 / §7.4)
 *
 * 表编码由 VVA_ENC_* 宏决定(include/vva_common.h:按架构默认 x86=B8、AArch64=A4,
 * C13 可互换)。vva_map_set 为对齐单字写(B8:64 位,A4:32 位),两架构均无撕裂,
 * 读者无锁(§7.4);并发多字段一致更新应通过"切换表指针"实现。
 * entry0 = 0 为"未映射"哨兵(§1.5):vva_map_init 全表清零后转换结果落在低端 4GB。
 */
#include "vva.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#if defined(VVA_ENC_B8)
uint64_t vva_map8[VVA_MAP_SIZE] __attribute__((aligned(VVA_TABLE_ALIGN)));
#define VVA_TBL     vva_map8
#define VVA_TBL_ESZ 8
#elif defined(VVA_ENC_A4)
uint32_t vva_map4[VVA_MAP_SIZE] __attribute__((aligned(VVA_TABLE_ALIGN)));
#define VVA_TBL     vva_map4
#define VVA_TBL_ESZ 4
#else
#error "VVA_ENC_B8 或 VVA_ENC_A4 必须二选一"
#endif

void *vva_table_base(void) {
    return (void *)(uintptr_t)VVA_TBL;
}

size_t vva_table_entry_bytes(void) {
    return VVA_TBL_ESZ;
}

void vva_map_init(void) {
    /* 全表清零:entry0 及所有未映射项 = 0(§1.5 哨兵) */
    memset((void *)(uintptr_t)VVA_TBL, 0, sizeof(VVA_TBL));
}

void vva_map_set(uint32_t idx, uint32_t H) {
    /* 断言(design §5.3):H < (1u << VVA_HIGH_BITS) 且不超过平台 W 上限(§1.4) */
    uint32_t wmax = (VVA_HIGH_BITS < VVA_W_MAX) ? VVA_HIGH_BITS : VVA_W_MAX;
    if (idx >= VVA_MAP_SIZE || H >= (1u << wmax)) {
        fprintf(stderr, "vva_map_set: 非法参数 idx=%u H=%u(有效 W 上限=%u)\n",
                idx, H, wmax);
        abort();
    }
#if defined(VVA_ENC_B8)
    VVA_TBL[idx] = (uint64_t)H << 32;   /* 预移位编码:B8(§2.1) */
#else
    VVA_TBL[idx] = H;                   /* 原始编码:A4,高位补零(§1.2) */
#endif
}

void vva_table_enable_hugepage(void) {
    /* 可选:表起始 2MB 对齐(VVA_TABLE_HP / make HP=1)时,把整表注册为
     * hugepage 候选,消除表访问自身的 dTLB 开销(test.md C12 / design §2.3)。 */
    uintptr_t a = (uintptr_t)VVA_TBL;
    if (a & ((2u << 20) - 1)) {
        fprintf(stderr, "vva_table: 未按 2MB 对齐(需 HP=1 构建),跳过 hugepage\n");
        return;
    }
    if (madvise((void *)a, sizeof(VVA_TBL), MADV_HUGEPAGE) != 0)
        fprintf(stderr, "vva_table: madvise(MADV_HUGEPAGE) 失败(THP 未开启?)\n");
}

/* 驻留寄存器初始化(design §5.3):配合 -ffixed-r15(x86,仅 GCC)/ -ffixed-x27(AArch64)。
 * 寄存器为 callee-saved,与 ABI 兼容,跨函数调用安全(§4.4)。
 *
 * ⚠ 实测约束(2026-11,Ubuntu glibc 2.39):glibc 的 __libc_start_main(call_init 内联)
 * 用 %r15 保存 init_array 迭代指针跨构造器调用传递,构造器内写 %r15 会使后续迭代
 * 读到表内存并 call 0(SIGSEGV)。因此构造器形式仅作 opt-in(VVA_TBL_CTOR),
 * 默认由框架在 main() 入口显式调用 vva_tbl_pin()——此时 libc 启动路径已结束,
 * 之后所有 libc 函数按 ABI 保留 r15/x27,安全。 */
void vva_tbl_pin(void) {
#if defined(__x86_64__)
    __asm__ volatile ("movq %0, %%r15" :: "r"((uint64_t)(uintptr_t)VVA_TBL) : "memory");
#elif defined(__aarch64__)
    __asm__ volatile ("mov x27, %0" :: "r"((uint64_t)(uintptr_t)VVA_TBL) : "memory");
#endif
}

#if defined(VVA_TBL_CTOR)
__attribute__((constructor)) static void vva_tbl_init(void) {
    vva_tbl_pin();
}
#endif
