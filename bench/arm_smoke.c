/* bench/arm_smoke.c —— AArch64 转换层交叉编译冒烟(run_all.sh 在非 ARM 主机上使用)
 *
 * 无 libc 依赖(vva.h 只引 stdint/stddef),可用
 *   clang --target=aarch64-linux-gnu -ffreestanding -O2 -c
 * 独立编译,验证 AArch64 代码序列的语法/寄存器分配/指令数契约。
 * 运行时正确性由 C0 在目标机把关(本机仅静态验证)。
 */
#include "vva.h"

#if !defined(__aarch64__)
#error "arm_smoke.c 仅用于 AArch64 目标(交叉编译)"
#endif

/* 指令数契约静态断言(design.md §8.1:AArch64 strict 4 / fast 3,±0)。
 * SEG / PURE 构建为分支型/编译器生成(VVA_CONV_INSN=0),不参与该断言。 */
#if !defined(VVA_SEG_CACHE) && !defined(VVA_VARIANT_PURE)
#if defined(VVA_TAG_FAST)
_Static_assert(VVA_CONV_INSN == 3, "AArch64 fast 转换应为 3 条(BFI)");
#else
_Static_assert(VVA_CONV_INSN == 4, "AArch64 strict 转换应为 4 条(UBFX/LDR/UBFX/ORR-LSL)");
#endif
#endif

void *arm_smoke_vva2va(uint64_t vva) { return vva2va(vva); }
void *arm_smoke_pure(uint64_t vva) { return vva2va_pure(vva); }
uint64_t arm_smoke_make(uint32_t idx, uint32_t off) { return VVA_MAKE(idx, off); }
