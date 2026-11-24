# design.md — VVA→VA 动态地址映射设计(x86-64 / AArch64)

> 依据 `readme.md`:设计一种软件动态地址映射,将应用指针(VVA)在每次 `ld/st` 前通过内联查表转换为真实虚地址(VA),并用最优指令序列实现。
> 本文档描述实现方式;测试框架与测试用例见 `test.md`(两种架构**转换代码分架构实现**,测试框架与用例完全共享)。关键序列已实际验证,见附录 A。

---

## 1. 地址模型与映射语义

### 1.1 VVA 位段定义

```
63            52 51             32 31              0
┌────────────────┬─────────────────┬─────────────────┐
│   tag(忽略)    │  idx(20 bit)   │  offset(32 bit) │    VVA(应用指针)
└────────────────┴─────────────────┴─────────────────┘
                    │ 内联查表                │ 直接保留
                    ▼                         ▼
┌──────────────────────────┬─────────────────────────┐
│  H = map[idx](W bit)     │  offset                 │    VA(真实虚地址)
└──────────────────────────┴─────────────────────────┘
 63                         32 31                      0
```

| 位段 | 名称 | 宽度 | 说明 |
|---|---|---|---|
| VVA[31:0] | offset | 32 | 直接成为 VA[31:0] |
| VVA[51:32] | idx | 20 | 映射表下标,表规模 2^20 = 1M 项 |
| VVA[63:52] | tag | 12 | **忽略**;实现必须保证 tag 任意取值不影响结果 |

### 1.2 映射公式与参数

```
VA = (map[VVA[51:32]] << 32) | VVA[31:0]
```

| 参数 | 含义 | 取值 |
|---|---|---|
| `VVA_IDX_BITS` | 索引宽度 | 固定 20(readme:位 51:32) |
| `VVA_HIGH_BITS`(W) | 表项产生的 VA 高位宽度 | ≤ 20;readme 字面语义为 VA[48:32](W = 17),canonical 48 位 VA 实际上限为 15/16(见 §1.4);**读法约定见 §1.6**,代码序列与 W 无关 |
| 表项数 | 2^20 | 每项存 VA 高位 H,高位补零 |

**表项约定**:H 一律存放在表项低位、高位补零(A 编码存 32 位字,B 编码存 `H<<32`)。这样**合并指令的位宽与 W 无关**,W 只影响表内容合法性,不影响代码生成。

### 1.3 无进位合并引理(为什么 `or`/`add`/`bfi` 等价)

设 `e = H << 32`(低 32 位全 0),`o = offset < 2^32`。则 `e + o` 在位 32 处不产生进位,故:

```
(H<<32) | o  ≡  (H<<32) + o  ≡  bfi 结果
```

该引理是所有最优序列的基础:无需先掩码 offset,可直接用 `or`/`add` 合并。

### 1.4 平台 W 上限(canonical/翻译约束)

转换结果的 VA 必须可被 MMU 翻译(保持 canonical),因此 W 受平台限制:

| 平台 | 用户态可用 W 上限 | 说明 |
|---|---|---|
| x86-64,4 级页表 | 15(VA[46:32]) | 用户 VA < 2^47,bit 47 恒 0 |
| x86-64,5 级页表(LA57) | 20(VA[51:32]) | 满足"52 位新地址" |
| AArch64,48 位 VA | 16(VA[47:32]) | TTBR0/TTBR1 在 2^48 处拆分 |
| AArch64,ARMv8.2-LVA(52 位 VA) | 20(VA[51:32]) | 满足"52 位新地址" |

构建时按平台 clamp W;`vva_map_set()` 对越界 H 断言失败。W=17(readme 字面值)要求 LA57 / LVA 平台。

### 1.5 保留值

`map[idx] == 0` 保留为"未映射"哨兵(转换结果落在低端 4GB,约定不可解引用)。分支无关的实现不做运行时检查,安全性由段分配器保证(见 §7)。

### 1.6 对 readme 字面表述的读法约定

readme 有两处表述需要在实现中显式约定(只影响表内容合法性,不影响任何指令序列):

1. **"48 位虚地址动态映射到 52 位新的地址"**:本设计取"真实 VA 为 48 位,应用可见的新地址空间为 52 位",即 VVA = 20 位索引 + 32 位 offset(位 0~51 全部有意义),tag 仅占 [63:52]。
2. **"将 VVA 高 51 到 32 位转换成真实的 VA 的高 48 到 32 位"**:字面为 17 位(VA[48:32]),但 48 位 VA 的高位区间实际是 [47:32](16 位),两者相差 1 位。处理方式:
   - **代码与 W 无关**(表项高位补零,`bfi` 固定插入 20 位、`or` 合并天然清零高位),因此该歧义不影响最优序列;
   - 严格按 canonical 48 位 VA 实现:W = 15(x86-64 L4)/ 16(AArch64 48 位 VA);
   - 按 readme 字面:W = 17,需 LA57 / ARMv8.2-LVA;
   - 按"52 位新地址"取满:W = 20,同样需 LA57 / LVA。
   - 框架默认 W = 17(字面),按平台 clamp(§1.4),并在测试结果中记录 W。

---

## 2. 映射表编码与布局

### 2.1 两种编码

| 编码 | 表项 | 表大小(2^20 项) | 每访问合并操作 | 适用 |
|---|---|---|---|---|
| **A4** | `uint32_t`,存 H(高位补零) | 4 MB | `shl 32` + `or`(x86)/ `bfi`(ARM) | **AArch64 默认** |
| **B8** | `uint64_t`,存 `H<<32`(预移位) | 8 MB | 单条 `or` | **x86-64 默认** |

### 2.2 默认选择的依据

- **x86-64 选 B8**:x86 无 BFI 类指令,合并需要 `shl` + `or` 两条;预移位编码省掉 `shl`,净省 1 条。代价是表翻倍(4→8 MB),但表驻留开销由热集决定(见 §8 与 test.md C4),全表大小只影响冷路径。
- **AArch64 选 A4**:`bfi` 直接把 4 字节表项插入位 32 起,天然保留低 32 位,与预移位编码指令数相同(见 §4);选 A4 使表减半,每条 cache line 容纳 16 项(B8 仅 8 项),对顺序 idx 模式更友好。
- 两种编码在两架构上**都保留为可测变体**(test.md C13),由实测数据验证默认选择。

### 2.3 布局要求

- 表起始 64B 对齐;可选 **2MB 对齐 + hugepage** 映射,消除表访问自身的 dTLB 开销(对 C4 随机索引场景重要)。
- `vva_map_set(idx, H)` 为唯一写入口;并发语义见 §7.3。

---

## 3. x86-64 转换实现(默认:B8 + strict)

### 3.1 表基址策略

| 策略 | 额外指令 | 说明 |
|---|---|---|
| **寄存器驻留(默认)** | 0 | 编译加 `-ffixed-r15`,汇编模板内**字面引用** `%r15`;r15 为 callee-saved,与 ABI 兼容。已验证 GCC;clang 21 x86-64 不支持 `-ffixed-*`(附录 A) |
| 全局寄存器变量 | 0 | `register uint64_t *tbl asm("r15")`,仅 GCC 支持(附录 A) |
| 绝对寻址 | 0 | `-fno-pie -no-pie -mcmodel=small`:`mov rax,sym(,%rsi,8)`(SIB base=101 的 disp32 绝对形式,已验证) |
| 每访问物化 | +1(仅孤立访问) | 表基址作为 `"r"` 操作数;循环内 `lea` 会被 LICM 外提到循环外 → 摊薄为 0(已验证) |

### 3.2 最优指令序列

输入:`rsi` = VVA,`r15` = 表基址,输出:`rdx` = VA。

**strict(tag 任意)— 5 条,4 有效 uop:**

```asm
mov   eax, esi              ; off = VVA[31:0]   ← 重命名消除,0 延迟,同时完成低 32 位掩码
shr   rsi, 32               ; rsi = VVA[63:32]
and   esi, 0xFFFFF          ; idx = VVA[51:32],清除 tag 残留(关键!)
mov   rdx, [r15 + rsi*8]    ; rdx = map[idx] = H<<32
or    rdx, rax              ; VA(低 32 来自 off,高 32 来自表项,[63:52] 恒为 0)
```

**fast(运行时约定 tag≡0)— 4 条,3 有效 uop:** 删除 `and` 行。tag 为零时 `shr` 后即为干净 idx。
若 tag 可能非零,`shr rsi,32` 后 tag 残留在 rsi[31:20],经 ×8 寻址放大为 `tag<<23` 的偏移,会命中表外错误地址甚至越界——**fast 版必须只在 tag≡0 契约下使用**。

**逐条开销(Intel Skylake+ / Zen3+,典型):**

| 指令 | uops | 延迟 | 说明 |
|---|---|---|---|
| `mov eax,esi` | 1(消除) | 0 | mov 消除,Ivy Bridge+/Zen+ |
| `shr rsi,32` | 1 | 1 | |
| `and esi,0xFFFFF` | 1 | 1 | 仅 strict;32 位写零扩展高位 |
| `mov rdx,[r15+rsi*8]` | 1 | ≈5(L1) | 依赖链核心;L2≈14,L3≈40-70,DRAM≈120-250 |
| `or rdx,rax` | 1 | 1 | 无进位合并(§1.3) |

- **关键路径**(表 L1 热):strict ≈ 1+1+5+1 = **8 cycles**,fast ≈ **7 cycles**,之后才是真正的 ld/st。
- **吞吐受限场景**(流式访存,ALU 端口有余量):额外 3~4 uop 大多被隐藏,预期净增 1~3 cycles/访问。

### 3.3 参考实现(GCC / clang 内联汇编)

```c
/* arch/x86_64/vva_impl.h —— 默认 B8 编码 */
extern uint64_t vva_map8[VVA_MAP_SIZE] __attribute__((aligned(64))); /* 表项 = H<<32 */

#if defined(VVA_BASE_PINNED)          /* 编译: -ffixed-r15(GCC;x86-64 clang 不支持) */
static inline void *vva2va(uint64_t vva) {
    uint64_t off, va;
    __asm__ volatile (
        "movl  %k[in], %k[off]         \n\t"  /* off = VVA[31:0] */
        "shr   $32, %[in]              \n\t"  /* in  = VVA[63:32] */
        "and   $0xFFFFF, %k[in]        \n\t"  /* in  = idx,忽略 tag */
        "movq  (%%r15,%[in],8), %[va]  \n\t"  /* 字面引用 r15:va = H<<32 */
        "orq   %[off], %[va]           \n\t"  /* va  = VA */
        : [va] "=&r"(va), [off] "=&r"(off), [in] "+r"(vva)
        :
        : "memory");
    return (void *)va;
}
#else                                  /* 基址作为操作数,由编译器物化/外提 */
static inline void *vva2va(uint64_t vva) {
    uint64_t off, va;
    __asm__ volatile (
        "movl  %k[in], %k[off]             \n\t"
        "shr   $32, %[in]                  \n\t"
        "and   $0xFFFFF, %k[in]            \n\t"
        "movq  (%[tbl],%[in],8), %[va]     \n\t"
        "orq   %[off], %[va]               \n\t"
        : [va] "=&r"(va), [off] "=&r"(off), [in] "+r"(vva)
        : [tbl] "r"(vva_map8)
        : "memory");
    return (void *)va;
}
#endif
/* fast 模式:删除 and 行(需 tag≡0 契约) */
```

注:`%k` 为 32 位子寄存器修饰符;`"=&r"` 早消失约束防止与输入共用寄存器;`"memory"` 阻止编译器跨越表更新重排(表静态时可用非 volatile 版本换取 CSE,见 §6)。驻留模式的寄存器初始化见 §5.3。

### 3.4 微架构注意事项

- **BMI2(x86-64-v3)**:`shr`+`and` 可替换为 `bextr`(控制值需常驻寄存器或确认编码),以及 `bzhi`/`shlx` 等价形式;是否更快由 test.md C13 实测裁决,不作为默认。
- 32 位写(`and esi`)之后读 64 位 `rsi` 作寻址:SnB+ 无部分寄存器停顿;Core2 及更早核有停顿,不在支持范围。
- `or` 合并可换 `add`(无进位引理保证等价),无优劣差异。

---

## 4. AArch64 转换实现(默认:A4 + strict)

### 4.1 表基址策略

ARM 没有绝对 disp32 寻址,基址必须先入寄存器:

| 策略 | 额外指令 | 说明 |
|---|---|---|
| **寄存器驻留(默认)** | 0 | 编译加 `-ffixed-x27`,汇编模板内**字面引用** `x27`;x27 为 callee-saved 且非平台保留寄存器。clang 已实测验证(附录 A);GCC 的 `-ffixed-*` 为标准文档化选项 |
| 每访问物化 | +2(仅孤立访问) | `adrp x16, sym; add x16, x16, :lo12:sym`;循环内会被外提,摊薄为 0 |
| 全局寄存器变量 | 0 | `register ... asm("x27")` 仅 GCC 支持(clang 直接报错,附录 A),不推荐作为唯一方案 |

### 4.2 最优指令序列

输入:`x0` = VVA,`x27` = 表基址,输出:`x0` = VA。

**strict(tag 任意)— 4 条,关键路径 ≈ 1+4+1 = 6 cycles(表 L1 热):**

```asm
ubfx  x1, x0, #32, #20          ; idx = VVA[51:32] —— UBFX 天然忽略 tag,无需掩码
ldr   w2, [x27, x1, lsl #2]     ; w2 = map[idx],32 位加载零扩展
ubfx  x3, x0, #0, #32           ; off = VVA[31:0](与上面的 ldr 并行发射)
orr   x0, x3, x2, lsl #32       ; VA = off | (H<<32),[63:52] 恒为 0
```

**fast(tag≡0 约定)— 3 条:**

```asm
ubfx  x1, x0, #32, #20
ldr   w2, [x27, x1, lsl #2]
bfi   x0, x2, #32, #20          ; x0[51:32] ← x2[19:0],保留 x0[31:0]
```

`bfi` 保留目标寄存器其余位,因此会保留 x0[63:52] 的 tag 残留——**fast 版要求 tag≡0**;同时由于表项高位补零,插入满 20 位后 x0[51:49] 为 0,W<20 时同样正确(strict 的 `orr` 版本对任意 W 亦正确)。所有指令均为 ARMv8.0 基线,**无需额外 ISA 扩展**(LVA 仅影响 W 上限)。

**逐条开销(Neoverse 级,典型):**

| 指令 | 延迟 | 说明 |
|---|---|---|
| `ubfx` | 1 | 两条 ubfx 可并行发射 |
| `ldr w,[x27,x1,lsl #2]` | ≈4(L1) | L2≈13,L3≈30-50,DRAM≈100-250 |
| `orr lsl #32` / `bfi` | 1 | |

### 4.3 参考实现(GCC / clang 内联汇编)

```c
/* arch/aarch64/vva_impl.h —— 默认 A4 编码 */
extern uint32_t vva_map4[VVA_MAP_SIZE] __attribute__((aligned(64))); /* 表项 = H */

#if defined(VVA_BASE_PINNED)          /* 编译: -ffixed-x27(GCC 与 clang 均支持) */
static inline void *vva2va(uint64_t vva) {
    uint64_t va, idx, base, off;
    __asm__ volatile (
        "ubfx  %[idx], %[vva], #32, #20             \n\t"
        "ldr   %w[base], [x27, %[idx], lsl #2]      \n\t"  /* 字面引用 x27 */
        "ubfx  %[off], %[vva], #0, #32              \n\t"
        "orr   %[va], %[off], %[base], lsl #32      \n\t"
        : [va] "=&r"(va), [idx] "=&r"(idx), [base] "=&r"(base), [off] "=&r"(off)
        : [vva] "r"(vva)
        : "memory");
    return (void *)va;
}
#else
static inline void *vva2va(uint64_t vva) {
    uint64_t va, idx, base, off;
    __asm__ volatile (
        "ubfx  %[idx], %[vva], #32, #20             \n\t"
        "ldr   %w[base], [%[tbl], %[idx], lsl #2]   \n\t"
        "ubfx  %[off], %[vva], #0, #32              \n\t"
        "orr   %[va], %[off], %[base], lsl #32      \n\t"
        : [va] "=&r"(va), [idx] "=&r"(idx), [base] "=&r"(base), [off] "=&r"(off)
        : [vva] "r"(vva), [tbl] "r"(vva_map4)
        : "memory");
    return (void *)va;
}
#endif
```

### 4.4 微架构注意事项

- `ldr w` 零扩展是 `orr`/`bfi` 正确性的前提(表项高位补零约定,§1.2)。
- B8 编码在 AArch64 无收益:`ldr x` + `orr`(无移位)与 A4 strict 同为 4 条,表却翻倍——这就是默认选 A4 的原因;仍保留为 C13 变体实测。
- `bfi` 版关键路径与 `orr` 版相同;`orr` 版的第二条 `ubfx` 与 `ldr` 并行,对乱序核更稳,故 strict 用 `orr` 版。
- x27 为 callee-saved,跨函数调用安全;但不要与平台保留寄存器(x18 等)混淆。

---

## 5. 统一接口与代码组织(转换分架构,框架共享)

### 5.1 目录结构

```
aemap/
├── design.md / test.md / readme.md
├── include/
│   ├── vva.h              # 公共宏接口(两架构一致)
│   └── vva_common.h       # 参数:VVA_IDX_BITS / VVA_HIGH_BITS / VVA_MAP_SIZE
├── arch/
│   ├── x86_64/vva_impl.h  # B8 编码 + x86 最优序列(§3)
│   └── aarch64/vva_impl.h # A4 编码 + ARM 最优序列(§4)
├── bench/                 # 测试框架(结构见 test.md §2,内核与架构无关)
└── Makefile               # ARCH= / ENC= / TAG= / BASE= / ISA= 变体矩阵
```

### 5.2 公共接口(`include/vva.h`)

```c
void *vva2va(uint64_t vva);                       /* 各架构各自实现,签名一致 */
void  vva_prefetch(uint64_t vva);                 /* 可选:prefetcht0 / prfm */

#define VVA_PTR(p)          ((void *)(uintptr_t)vva2va((uint64_t)(p)))
#define VVA_LD(type, p)     (*(type *)(uintptr_t)VVA_PTR(p))
#define VVA_ST(type, p, v)  (VVA_LD(type, p) = (v))
```

派发仅在头文件包含层发生,内核与框架代码 `#include "vva.h"` 即可,不含任何 `#if defined(__x86_64__)` 分支:

```c
#if defined(__x86_64__)
  #include "arch/x86_64/vva_impl.h"
#elif defined(__aarch64__)
  #include "arch/aarch64/vva_impl.h"
#else
  #error unsupported arch
#endif
```

### 5.3 表初始化与驻留寄存器初始化

```c
void vva_map_init(void);                       /* 全表清零 + 预映射 */
void vva_map_set(uint32_t idx, uint32_t H);    /* A4: map4[idx]=H; B8: map8[idx]=(uint64_t)H<<32 */
                                               /* 断言 H < (1u << VVA_HIGH_BITS) 且 ≤ 平台上限 */

/* VVA_BASE_PINNED 构建:启动时初始化保留寄存器(配合 -ffixed-r15 / -ffixed-x27) */
__attribute__((constructor))
static void vva_tbl_init(void) {
#if defined(__x86_64__)
    __asm__ volatile ("movq %0, %%r15" :: "r"((uint64_t)vva_map8) : "r15");
#elif defined(__aarch64__)
    __asm__ volatile ("mov x27, %0" :: "r"((uint64_t)vva_map4));
#endif
}
```

### 5.4 构建矩阵

| 变量 | 取值 | 默认 |
|---|---|---|
| `ARCH` | `x86_64` / `aarch64` | `uname -m` |
| `ENC` | `A4` / `B8` | x86_64→B8,AARCH64→A4 |
| `TAG` | `strict` / `fast` | strict |
| `BASE` | `pinned` / `mat` / `abs`(仅 x86) | pinned |
| `ISA` | x86: `v1` / `v3`(BMI2);ARM: `armv8-a` | v1 / armv8-a |

关键编译选项:寄存器驻留构建加 `-ffixed-r15`(x86,仅 GCC;x86-64 clang 不支持)或 `-ffixed-x27`(AArch64,clang 已验证、GCC 为标准选项);绝对寻址构建需 `-fno-pie`(编译)+ `-no-pie`(链接)+ `-mcmodel=small`。无保留寄存器时回退到操作数物化形式(循环内摊薄为 0,附录 A)。

---

## 6. 可选优化变体(供 test.md C13 对比,均非默认)

| 变体 | 架构 | 思路 | 预期收益场景 |
|---|---|---|---|
| `fast` | 两者 | 约定 tag≡0,省 1 条 | 任何场景 −1 insn / −1 cycle |
| `enc` 互换 | 两者 | x86→A4 / ARM→B8 | 实测验证 §2.2 的取舍 |
| `bextr`(v3) | x86 | 提取压缩为 1 条(控制值常驻寄存器) | −1 uop,视微架构 |
| `mat` 基址物化 | 两者 | 每访问 `lea`/`adrp+add` | 循环内摊薄为 0;仅孤立访问有代价 |
| `pf` 表项预取 | 两者 | 对下一访问的表项 `prefetcht0`/`prfm`(地址序列预生成,软件流水) | 表 miss 高的顺序/跨段模式 |
| `cache` 段缓存 | 两者 | `idx == last` 时跳过查表(分支预测型) | idx 高度重复(C2/C3 类) |
| `gs` GS-base | 仅 x86 | 段不切换期间把基址写入 GS base,访存用 `%gs:off32`,**0 条转换指令**;切换时 `wrgsbase`(~20-40c,需 FSGSBASE + 内核 ≥5.9;glibc TLS 占用 FS,故用 GS) | 段粘性强的吞吐场景 |
| 纯 C 版 | 两者 | `((uint64_t)map[(vva>>32)&0xFFFFF]<<32) \| (uint32_t)vva` 交编译器生成 | 对照:验证手写序列是否优于编译器(索引必须掩码 tag) |

补充:**非 volatile 的 `vva2va_pure()`**(无 `"memory"` clobber)允许编译器对相同输入做 CSE,适合一个基本块内多次访问同一指针的代码;默认 volatile 版本语义安全。

---

## 7. 正确性与运行时约束

1. **无进位合并**(§1.3)是所有序列的正确性基础;表项高位必须补零。
2. **tag 忽略**:strict 序列经 UBFX/掩码保证 tag 任意值结果不变;fast 序列是显式契约(tag≡0),框架 C0 对两种模式分别验证。
3. **对齐与跨段规则**(段分配器契约):
   - 对象不得跨越 2^32 offset 边界,除非其后续所需基址与当前基址连续(基址 = 连续整数)——否则一次对象访问会跨两个表项,分支无关实现不会检查;
   - 转换不做边界检查,安全性由分配器保证。
4. **并发**:表为读多写少;`vva_map_set` 为对齐单字写(A4:32 位,B8:64 位),两架构均无撕裂,读者无锁。写者如需多字段一致更新,应通过"切换表指针"实现,而非逐项更新。
5. **未映射**:entry 0 哨兵语义(§1.5);strict 分支无关实现不产生陷阱,由上层按需引入检查型变体。

---

## 8. 开销预算与预期(与 test.md 实测对照)

### 8.1 指令增量(相对裸 ld/st)

| 变体 | x86-64 | AArch64 |
|---|---|---|
| strict + pinned | 5 条(4 有效 uop) | 4 条 |
| fast + pinned | 4 条(3 有效 uop) | 3 条 |
| + 每访问物化基址 | +1(`lea`,仅孤立访问;循环内摊薄 0) | +2(`adrp+add`,同上) |
| 编码互换 | A4:+1(`shl`) | B8:±0(表 ×2) |

### 8.2 延迟模型

转换的表加载与数据访问**地址相关、串行**:

```
每次转换访问延迟 ≈ t_extract(1) + t_table_load(L) + t_merge(1) + t_data_load
```

- 吞吐受限(流式):乱序核可将相邻访问的表加载与数据加载重叠,增量 ≈ 额外 uop 的端口占用(1~3 c/op);
- 延迟受限(指针追逐链):增量 ≈ 完整 `1 + L + 1`,L 为表加载命中层级延迟。

### 8.3 预期 Δ(设计目标,数值由 test.md 实测校准)

| 表状态 | 依赖链 Δ | 吞吐型 Δ |
|---|---|---|
| L1 热 | +6~8 c | +1~3 c |
| L2 热 | +15~20 c | +3~6 c |
| L3 热 | +35~60 c | +8~15 c |
| DRAM(全冷) | +100~250 c | 视 MLP 并发度 |

指令数增量必须与 §8.1 精确一致(框架硬校验);两架构同用例横向对比由共享框架直接产出。

---

## 附录 A:实现验证记录(2026-11-25)

环境:GCC 15.2.0(x86-64);Clang 21.1.8(AArch64 交叉汇编,无交叉运行环境)。

| 验证项 | 方法 | 结果 |
|---|---|---|
| x86-64 strict 正确性 | 10^6 随机 (idx, offset, tag∈[0,2^12)),对参考 C 实现 | 0 失配;tag 不变性 0 违例 |
| x86-64 fast 正确性 | 同上,tag≡0 | 0 失配 |
| x86-64 指令条数 | `gcc -O2 -S` | strict 5 条 / fast 4 条,与 §3.2 一致 |
| x86-64 驻留模式 | `-ffixed-r15` + 字面 `%r15` | 5 条,无 `lea`;功能正确 |
| x86-64 物化开销外提 | `gcc -O2 -S` 观察循环内核 | `lea map(%rip)` 位于循环外(§3.1 修正依据) |
| x86-64 绝对寻址变体 | `gcc -fno-pie -no-pie`,`movq map(,%rdi,8)` | 汇编/运行正确(结果 0x1ffff00000007) |
| AArch64 strict/fast 语法与条数 | `clang --target=aarch64-linux-gnu -O2 -S` | strict 4 条(UBFX/LDR/UBFX/ORR-LSL)、fast 3 条,操作数合法 |
| AArch64 驻留模式 | `-ffixed-x27` + 字面 `x27` | 无 x27 保存/恢复,4 条 |
| AArch64 驻留寄存器初始化(§5.3) | `mov x27, %0` 构造器片段,clang 交叉汇编 | 通过 |
| 编译器兼容性 | clang 21 | 全局寄存器变量在 x86-64 与 AArch64 上均直接报错(仅 GCC 支持);x86-64 不支持 `-ffixed-*`(GCC 支持);AArch64 支持 `-ffixed-x27` |

说明:AArch64 仅完成语法/指令数/语义级验证,动态正确性由 test.md C0 在目标机执行把关;x86-64 已完成动态运行验证(含 tag 扰动)。
