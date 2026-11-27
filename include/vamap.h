/*
 * vamap.h — VVA→VA 动态地址映射：公共 API、表项编码、内联转换快路径。
 *
 * 设计见 design.md：
 *   - VVA 布局：[63:52] tag（默认显式掩码；fast 变体约定为 0）、
 *               [51:32] 映射索引 idx（2^VAMAP_IDX_BITS 个表项）、[31:0] 区域内偏移。
 *   - 表项编码：entry = arena + (region << 32)，低 32 位恒 0；
 *               entry == 0 ⇔ 无效；VA = entry | (uint32_t)vva。
 *   - 快路径：acquire load + 判零分支 + 零扩展 + OR 合并；
 *               无效时调用 vamap_slow_create_fill()（慢路径创建并填充表项）。
 */
#ifndef VAMAP_H
#define VAMAP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 构建期配置                                                          */
/* ------------------------------------------------------------------ */

/* 映射索引位宽（默认 20 → 2^20 = 1M 表项，表长 8 MiB）。改动时同步更新
 * test.md/bench 的掩码与规模；融合汇编按默认 20 位写死立即数。 */
#ifndef VAMAP_IDX_BITS
#define VAMAP_IDX_BITS 20
#endif
#define VAMAP_NIDX     (1u << VAMAP_IDX_BITS)
#define VAMAP_IDX_MASK (VAMAP_NIDX - 1u)

/* fast 变体：约定 tag（bit[63:52]）≡ 0，快路径省去 andl 掩码。
 * 默认（strict）显式掩码，满足 readme 的“52 位及以上忽略”。 */
/* #define VAMAP_FAST_TAG0 1 */

/* 快路径新增指令数（每访存），供测试框架 instr_per_op 断言使用
 * （test.md §9.3）：x86-64 strict 8 / fast 7（无 BMI2 基线，含保留 vva
 * 的 movq）；ARM64 为 5。 */
#if defined(__x86_64__)
#  if defined(VAMAP_FAST_TAG0)
#    define VAMAP_CONV_INSNS 7
#  else
#    define VAMAP_CONV_INSNS 8
#  endif
#elif defined(__aarch64__)
#  define VAMAP_CONV_INSNS 5
#else
#  define VAMAP_CONV_INSNS 0
#endif

/* 时延模拟模式 */
#define VAMAP_LATENCY_SPIN  0   /* rdtsc 校准忙等（默认） */
#define VAMAP_LATENCY_SLEEP 1   /* clock_nanosleep 让核 */

typedef uint64_t vva_t;         /* 52 位 VVA；默认实现忽略 bit[63:52] */

/* ------------------------------------------------------------------ */
/* 公共 API（design.md §6.1）                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t create_latency_ns;   /* 映射建立时延；测试取 10us/100us/1ms/10ms */
    int      latency_mode;        /* VAMAP_LATENCY_SPIN(默认) / VAMAP_LATENCY_SLEEP */
    uint32_t lock_stripes;        /* 慢路径锁条带数，默认 4096，须为 2 的幂 */
    uint32_t alias_pool_size;     /* bench alias 模式：预留 [1, n] 个 region 供
                                     alias/create 复用；0=禁用（默认），须 < REGION_COUNT */
    int      strict_48bit;        /* 置位时区域号限制在 [1, 2^16-1]，并叠加
                                     arena 上界 floor((2^48-arena)/4GiB)-1 与 REGION_COUNT-1 */
} vamap_cfg;

int      vamap_init(const vamap_cfg *cfg);
void     vamap_shutdown(void);     /* 释放映射表与 arena；进程退出前可省略 */

vva_t    vamap_vva_alloc(uint32_t nidx);  /* 预留 n 个连续索引（不建映射）；成功返回基址 VVA (idx<<32)，失败返回 (vva_t)-1 */
void     vamap_vva_free(vva_t base, uint32_t nidx);

int      vamap_map_create(uint32_t idx);  /* 0=成功/已存在；非0=失败；仅实际新建时含模拟时延 */
int      vamap_map_destroy(uint32_t idx); /* 表项置 0；引用计数归零时回收区域号（无模拟时延） */
int      vamap_map_alias(uint32_t idx, uint32_t region);
                                           /* 仅对当前无效 idx 预填到已有 region
                                              （引用计数 +1），不计时延；已有效或
                                              region 不可用时返回非 0 */

void     vamap_set_create_latency_ns(uint64_t ns);
void    *vamap_pin(uint64_t vva);         /* 返回当前 4GiB 窗口真实基址（无效时先创建；供循环提升查表） */

typedef struct {
    uint64_t creates, destroys;           /* 慢路径事件计数（不含 setup 预填；校验用） */
    uint64_t spin_ns;                     /* 累计模拟时延（仅 create 的 latency 部分） */
    uint64_t slow_ns_total;               /* 全局累计（多线程求和；含锁、分配、模拟时延） */
    uint64_t slow_calls;                  /* 进入慢路径的次数（含 double-check 命中） */
} vamap_stats;
vamap_stats vamap_get_stats(void);
void     vamap_reset_stats(void);   /* 框架扩展：预热前清零计数（test.md §4.2） */

/* 运行时元数据（bench 记录 region_count / arena_base / 表驻留方式） */
typedef struct {
    uint32_t region_count;              /* 实际回退后的 REGION_COUNT（arena 规模） */
    uint32_t region_limit;              /* 可分配 region 号上界（含 strict 收缩） */
    uint32_t alias_pool_size;           /* 实际生效的池大小 */
    uint64_t arena_base;                /* 竞技场 4GiB 对齐基址 */
    char     table_backing[32];         /* 映射表驻留方式：hugetlb/thp/default */
} vamap_info_t;
vamap_info_t vamap_get_info(void);

/* 慢路径（快路径内联代码调用；亦为 §5.6 融合汇编的 call 目标） */
void *vamap_slow_create_fill(uint64_t vva);

/* TSC 标定（bench 计时借用；进程内首次 init 时标定一次） */
double vamap_tsc_hz(void);

/* 快路径读侧共享状态：init 后只被慢路径 release 写、快路径 acquire 读 */
extern uint64_t *vamap_table;
extern uint64_t  vamap_arena;            /* 4GiB 对齐竞技场基址 */

/* ------------------------------------------------------------------ */
/* C 参考快路径（design.md §5.1，默认 strict：显式掩掉 tag）            */
/* ------------------------------------------------------------------ */

static inline __attribute__((always_inline, hot))
void *vva2va(uint64_t vva)
{
    uint32_t idx = (uint32_t)(vva >> 32)
#if !defined(VAMAP_FAST_TAG0)
        & VAMAP_IDX_MASK
#endif
        ;
    uint64_t e = __atomic_load_n(&vamap_table[idx], __ATOMIC_ACQUIRE);
    if (__builtin_expect(e == 0, 0))
        return vamap_slow_create_fill(vva);   /* 无效：运行时创建并填充表项 */
    return (void *)(e | (uint32_t)vva);       /* 合并：VA 高位 | 零扩展 offset */
}

/* ------------------------------------------------------------------ */
/* 融合访存宏（design.md §5.6）：转换 + 实际 ld/st 一体化，             */
/* volatile asm 禁止编译器对同一地址的转换做 CSE 或提升到循环外。        */
/* 仅在默认 20 位索引位宽下提供汇编版；其余走 C 回退。                  */
/* ------------------------------------------------------------------ */

#if VAMAP_IDX_BITS == 20 && (defined(__x86_64__) || defined(__aarch64__))

#if defined(__x86_64__)
/*
 * x86-64：输入操作数会被分配到 callee-saved 寄存器（clobber 覆盖全部
 * caller-saved），call 慢路径后仍有效。构建须加 -mno-red-zone（asm 内含
 * call，编译器按 leaf 使用 128B red zone 会被 callee 覆盖）。
 * 注意合并必须“零扩展 + OR”两步，不能写 %eax（32 位写清零高位）。
 * 慢路径调用前将 rsp 16 字节对齐（编译器按 leaf 处理 volatile asm，
 * 不保证 call 点对齐）。对齐期间的 rsp 暂存于 rbx：rbx 是 callee-saved，
 * 跨 call 存活；GCC 的 rbx 值先 push 到栈上保护、返回后恢复。
 * 绝不可用 caller-saved 寄存器（r8–r11 等）暂存——被调函数可随意改写。
 * 操作数编号：%0 = 数据（load 输出 / store 输入），%1 = vva，%2 = 表基址。
 *
 * XMM 必须列入 clobber：慢路径是普通 C 调用，按 SysV ABI 可改写全部
 * xmm0–15（double 返回值就在 xmm0）。若漏列，调用方跨 asm 存活的 FP
 * 常量/中间值（如 RNG 的 2^-53 换算）会被静默污染——曾致 T9 的 zipf
 * 分布在首次慢调用后退化为常数 NaN→key 恒 255。
 */
#define VAMAP_X86_CLOBBER                                                \
    : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",       \
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",    \
      "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14",       \
      "xmm15", "cc", "memory"

#define VAMAP_X86_SLOWCALL                                               \
    "movq  %1, %%rdi             \n\t" /* 先取实参（操作数仍完好） */    \
    "pushq %%rbx                 \n\t" /* 保护 GCC 的 rbx（可能占用于   \
                                            操作数） */                   \
    "movq  %%rsp, %%rbx          \n\t" /* rbx 跨 call 存活 */           \
    "andq  $-16, %%rsp           \n\t" /* call 点 16 对齐（ABI） */     \
    "call  vamap_slow_create_fill\n\t" /* rdi = vva；rax = VA */        \
    "movq  %%rbx, %%rsp          \n\t" /* 恢复栈指针 */                 \
    "popq  %%rbx                 \n\t" /* 恢复 rbx */                   \
    "jmp   2f                    \n\t" /* 冷路径：跳过合并 */

#define VAMAP_X86_LOAD(ldop, qk)                                         \
    __asm__ volatile (                                                   \
    "movq  %1, %%rcx             \n\t"                                   \
    "shrq  $32, %%rcx            \n\t"                                   \
    "andl  $0xFFFFF, %%ecx       \n\t"   /* idx，忽略 tag */              \
    "movq  (%2,%%rcx,8), %%rax   \n\t"   /* entry（唯一一次表 load） */   \
    "testq %%rax, %%rax          \n\t"                                   \
    "jne   1f                    \n\t"   /* 有效：跳合并；无效落慢路径 */ \
    VAMAP_X86_SLOWCALL                                                   \
    "1: movl  %k1, %%ecx         \n\t"   /* ecx = 零扩展 offset */       \
    "orq   %%rcx, %%rax          \n\t"   /* rax = VA */                  \
    "2: " ldop " (%%rax), " qk "0 \n\t"  /* 实际 load */                  \
    : "=&r"(v) : "r"(vva), "r"(tbl) VAMAP_X86_CLOBBER)

#define VAMAP_X86_STORE(stop, k)                                         \
    __asm__ volatile (                                                   \
    "movq  %1, %%rcx             \n\t"                                   \
    "shrq  $32, %%rcx            \n\t"                                   \
    "andl  $0xFFFFF, %%ecx       \n\t"                                   \
    "movq  (%2,%%rcx,8), %%rax   \n\t"                                   \
    "testq %%rax, %%rax          \n\t"                                   \
    "jne   1f                    \n\t"                                   \
    VAMAP_X86_SLOWCALL                                                   \
    "1: movl  %k1, %%ecx         \n\t"                                   \
    "orq   %%rcx, %%rax          \n\t"                                   \
    "2: " stop " " k "0, (%%rax) \n\t"   /* 实际 store */                 \
    : : "r"(x), "r"(vva), "r"(tbl) VAMAP_X86_CLOBBER)

static inline __attribute__((always_inline))
uint64_t vload64(uint64_t vva)
{
    uint64_t v;
    uint64_t tbl = (uint64_t)(uintptr_t)vamap_table;
    VAMAP_X86_LOAD("movq", "%q");
    return v;
}

static inline __attribute__((always_inline))
uint32_t vload32(uint64_t vva)
{
    uint32_t v;
    uint64_t tbl = (uint64_t)(uintptr_t)vamap_table;
    VAMAP_X86_LOAD("movl", "%k");
    return v;
}

static inline __attribute__((always_inline))
void vstore64(uint64_t vva, uint64_t x)
{
    uint64_t tbl = (uint64_t)(uintptr_t)vamap_table;
    VAMAP_X86_STORE("movq", "%q");
}

static inline __attribute__((always_inline))
void vstore32(uint64_t vva, uint32_t x)
{
    uint64_t tbl = (uint64_t)(uintptr_t)vamap_table;
    VAMAP_X86_STORE("movl", "%k");
}

#else /* __aarch64__ */
/*
 * ARM64：ubfx 天然取 [51:32]；LDAR 不支持比例变址，先算地址；
 * 合并用 bfi 保留表项高位（不能 mov w，32 位写清零高位）。
 * clobber 全部 caller-saved 通用寄存器 + lr，输入/输出落到 x19–x28。
 * AAPCS 要求 sp 恒 16 字节对齐（GCC 全程维持该不变量），bl 点无需重对齐；
 * lr(x30) 已列入 clobber（bl 会改写）。
 * SIMD/FP 寄存器 v0–v31 同为 caller-saved（double 返回在 v0），必须列入
 * clobber——漏列会静默污染调用方跨 bl 存活的 FP 状态（同 x86 的教训）。
 * 操作数编号：%0 = 数据（load 输出 / store 输入），%1 = vva，%2 = 表基址。
 */
#define VAMAP_ARM_CLOBBER                                                \
    : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",        \
      "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x30",     \
      "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9",        \
      "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18",     \
      "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27",     \
      "v28", "v29", "v30", "v31", "memory"

#define VAMAP_ARM_SLOWCALL                                               \
    "mov  x0, %1                 \n\t"                                   \
    "bl   vamap_slow_create_fill \n\t"   /* x0 = vva；x0 = VA */         \
    "mov  x10, x0                \n\t"                                   \
    "b    2f                     \n\t"

#define VAMAP_ARM_LOAD(ldop, w)                                          \
    __asm__ volatile (                                                   \
    "ubfx x9,  %1, #32, #20      \n\t"   /* idx = VVA[51:32] */           \
    "add  x10, %2, x9, lsl #3    \n\t"   /* LDAR 无比例变址 */            \
    "ldar x10, [x10]             \n\t"   /* acquire load */              \
    "cbnz x10, 1f                \n\t"                                   \
    VAMAP_ARM_SLOWCALL                                                   \
    "1: bfi x10, %1, #0, #32     \n\t"   /* 低 32 位插入 offset */        \
    "2: " ldop " " w "%0, [x10]  \n\t"   /* 实际 load */                  \
    : "=&r"(v) : "r"(vva), "r"(tbl) VAMAP_ARM_CLOBBER)

#define VAMAP_ARM_STORE(stop, w)                                         \
    __asm__ volatile (                                                   \
    "ubfx x9,  %1, #32, #20      \n\t"                                   \
    "add  x10, %2, x9, lsl #3    \n\t"                                   \
    "ldar x10, [x10]             \n\t"                                   \
    "cbnz x10, 1f                \n\t"                                   \
    VAMAP_ARM_SLOWCALL                                                   \
    "1: bfi x10, %1, #0, #32     \n\t"                                   \
    "2: " stop " " w "%0, [x10]  \n\t"   /* 实际 store */                 \
    : : "r"(x), "r"(vva), "r"(tbl) VAMAP_ARM_CLOBBER)

static inline __attribute__((always_inline))
uint64_t vload64(uint64_t vva)
{
    uint64_t v;
    uint64_t tbl = (uint64_t)(uintptr_t)vamap_table;
    VAMAP_ARM_LOAD("ldr", "%");
    return v;
}

static inline __attribute__((always_inline))
uint32_t vload32(uint64_t vva)
{
    uint32_t v;
    uint64_t tbl = (uint64_t)(uintptr_t)vamap_table;
    VAMAP_ARM_LOAD("ldr", "%w");
    return v;
}

static inline __attribute__((always_inline))
void vstore64(uint64_t vva, uint64_t x)
{
    uint64_t tbl = (uint64_t)(uintptr_t)vamap_table;
    VAMAP_ARM_STORE("str", "%");
}

static inline __attribute__((always_inline))
void vstore32(uint64_t vva, uint32_t x)
{
    uint64_t tbl = (uint64_t)(uintptr_t)vamap_table;
    VAMAP_ARM_STORE("str", "%w");
}

#endif /* arch */

#else /* 通用 C 回退 */

static inline __attribute__((always_inline))
uint64_t vload64(uint64_t vva) { return *(volatile uint64_t *)(uintptr_t)vva2va(vva); }
static inline __attribute__((always_inline))
uint32_t vload32(uint64_t vva) { return *(volatile uint32_t *)(uintptr_t)vva2va(vva); }
static inline __attribute__((always_inline))
void vstore64(uint64_t vva, uint64_t x) { *(volatile uint64_t *)(uintptr_t)vva2va(vva) = x; }
static inline __attribute__((always_inline))
void vstore32(uint64_t vva, uint32_t x) { *(volatile uint32_t *)(uintptr_t)vva2va(vva) = x; }

#endif

/* ------------------------------------------------------------------ */
/* 应用侧集成宏（design.md §8）                                        */
/* ------------------------------------------------------------------ */

#define VLOAD(type, p)     (*(type *)(uintptr_t)vva2va((uint64_t)(uintptr_t)(p)))
#define VSTORE(type, p, x) (*(type *)(uintptr_t)vva2va((uint64_t)(uintptr_t)(p)) = (x))

#ifdef __cplusplus
}
#endif

#endif /* VAMAP_H */
