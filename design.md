# design.md — VVA→VA 动态地址映射设计与实现

> 本文档描述 readme.md 项目目标的实现方式：地址格式与映射模型、映射表与表项编码、
> 最优的查表转换指令、mock 运行时库（映射创建/销毁/建立时延模拟）、线程安全设计与应用集成方式。
> 测试框架与测试用例在 [test.md](test.md) 中详细描述。

## 1. 概述

应用代码使用 52 位的"虚虚地址" **VVA**（Virtual Virtual Address）作为指针：

- 低 32 位 `[31:0]` 为区域内偏移（offset）；
- `[51:32]` 为映射索引（idx），索引映射表；
- `[63:52]` 忽略（默认实现显式掩码；fast 变体约定为 0）。

每次 `ld`/`st` 之前进行一次**内联查表**：用 `VVA[51:32]` 查映射表得到区域真实基址
（VA 高位字，包含 `VA[48:32]` 区域字段），与 `VVA[31:0]` 合并成 VA，再用 VA 完成实际访存。
查表后判断表项是否有效；
无效时走慢路径：调用运行时中的映射创建函数（含可配置建立时延）并**填充表项**。
映射表被多线程共享，表项的创建与发布必须线程安全。

设计目标：

1. **快路径指令数最少**：每次 ld/st 仅新增一次表项 load 与少量 ALU/分支指令；
2. **慢路径语义清晰**：同索引创建串行化、结果原子发布、建立时延可配置为 10us/100us/1ms/10ms；
3. **线程安全**：快路径无锁读，慢路径加锁串行化 + release 发布；
4. **可度量**：转换开销能被 test.md 的框架精确测量并分解为快路径/慢路径两部分。

## 2. 地址格式与映射模型

### 2.1 VVA（应用指针）布局

| 位段 | 宽度 | 含义 |
|------|------|------|
| `[63:52]` | 12 | 未使用/忽略（tag）：转换结果不得依赖这些位；默认实现显式掩码，fast 变体约定为 0 |
| `[51:32]` | 20 | 映射索引 idx，映射表下标，共 `2^20 = 1M` 个表项 |
| `[31:0]`  | 32 | 区域内偏移 offset（区域粒度 4 GiB） |

VVA 空间 = `2^20` 个 4 GiB 区域 = `2^52` 字节。

### 2.2 VA（真实虚地址）布局

| 位段 | 宽度 | 含义 |
|------|------|------|
| `[63:32]` | 32 | 区域基址的 `[63:32]` 位：`(arena + (region << 32)) >> 32`（基址低 32 位恒 0） |
| `[31:0]`  | 32 | 直接取 `VVA[31:0]` |

`arena` 是 4 GiB 对齐的竞技场基址；`region` 是相对竞技场的区域号。readme 的
`VA[48:32]`（17 位）是 49 位 VA 描述下的区域字段；本实现把**完整区域基址**写入表项，
因此同样适用于 LA57 / ARMv8.2-LVA 等更宽的真实 VA。

### 2.3 映射关系

```
VA = (arena + (region(idx) << 32)) | (VVA & 0xFFFFFFFF)
VA = entry(idx) | (VVA & 0xFFFFFFFF)          entry(idx) = arena + (region(idx) << 32)
region : 索引 → [1, REGION_COUNT)             由运行时动态分配（region 0 保留，允许多对一）
entry(idx) == 0 表示该索引尚未建立映射
```

- 映射允许多对一：多个 idx 可指向同一 region；区域号按需分配，同时活跃的映射数
  受 region 号空间限制（相对 region 号 17 位为 `2^17−1`，严格 48 位模式为 `2^16−1`；
  实际可用数还受平台用户 VA 上限约束，见 §6.2 的 `REGION_COUNT` 回退表）；
- 表项数 `2^20` 大于可用 region 数，因此“全表有效”只能通过多对一预填（alias）实现；
  正常运行路径仍按需分配/复用 region；
- **运行时保留 region 0 不分配**，且有效表项为 `arena + region<<32`（arena 4 GiB 对齐、
  region ≥ 1），因此有效表项恒非 0；"表项整字为 0 ⇔ 无效"成立，无需独立 valid 位；
- 严格 48 位虚地址模式下，相对区域号分配限制在 `[1, 2^16−1]`，并要求
  `arena + (最大已分配 region 号 + 1)×4 GiB ≤ 2^48`：最高可访问地址上界为 `2^48−1`；
  因 arena 通常非 0，可用 region 号上界为
  `min(2^16−1, floor((2^48 − arena)/4 GiB) − 1)`，并非总能取满 `2^16−1`
  （区域基址上界 `256 TiB − 4 GiB` 仅在 arena=0 时取到）。用户态可访问空间随平台
  而定——ARM64 48 位 VA 为完整 256 TiB，x86-64 4 级页表用户空间仅 128 TiB，见 §6.2；
  17 位相对 region 号宽度保留，便于将来扩展到 `2^49` 地址空间。两种取法对快路径
  指令形态没有影响。

> **规格说明**：readme 原文为"将 VVA 高 51 到 32 位转换成真实的 VA 高 48 到 32 位"。
> 本设计用表项存完整区域基址，相对 region 号取 17 位（对应 `VA[48:32]` 的字段宽度）；
> 若严格要求真实 VA 为 48 位，只需将相对 region 号上限设为 `2^16−1` 并保证
> `arena + 竞技场 ≤ 2^48`，快路径不变。

### 2.4 表项编码（核心设计决策）

```c
/* 表项：64 位，天然 8 字节对齐，可原子读写 */
entry = (uint64_t)arena + ((uint64_t)region << 32);
/* bit[31:0] = 0；bit[63:32] = arena 基址区域 + region（即 VA 高位字） */
/* entry == 0  ⇔  无效；  entry != 0 ⇔ 有效，VA = entry | (uint32_t)vva */
```

这样编码的收益直接决定快路径形态：

1. **完整基址预置到位**：表项值即 VA 的高位字；与 offset 合并时先把 offset
   零扩展，再用一条 OR 合并（x86：`movl %edi, %ecx` + `orq %rcx, %rax`），
   无需移位、额外加法或掩码；
2. **无独立 valid 位**：有效性判断 = 整字判零（`test rax, rax`），不需要任何掩码；
3. **低位全 0**：arena 与 region 基址均 4 GiB 对齐，`entry | offset` 精确无损；
4. **8 字节对齐**：单条 mov 即原子读写，发布/读取无需复杂协议。

## 3. 系统总体结构

```
应用代码（VVA 指针）
    │  VLOAD / VSTORE 宏（每次访存前内联转换）
    ▼
快速路径 vva2va()  ──entry==0──▶  慢路径 vamap_slow_create_fill()
    │ 查映射表(8 MiB)                  │ 条带锁 + double-check
    ▼                                  ▼
实际 ld/st（VA）                 mock 运行时：区域分配 + 建立时延模拟
                                       │ release 发布表项
                                       ▼
                                  映射表（多线程共享）
```

目录结构：

```
include/vamap.h            # 公共 API、表项编码、内联转换、平台选择
src/vamap_runtime.c        # 运行时：init/创建/销毁/时延模拟/区域与锁管理
src/vamap_arch.c           # （可选）手写汇编快路径
bench/                     # 测试框架（见 test.md）：harness / patterns / state / timer / runtime / report
tests/                     # 正确性套件（见 test.md §5）
scripts/check_fastpath.sh  # objdump 校验快路径指令形态
design.md  test.md
```

## 4. 映射表设计

- 定义：`uint64_t *vamap_table`，指向 `vamap_init` 分配的 **8 MiB** 表（`2^20` 个 8 B 表项）；
- `init` 时用匿名 `mmap`（`PROT_READ|PROT_WRITE`、`MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE`）
  分配，并以 2 MiB 对齐/建议大页驻留（`MAP_HUGETLB` 或 `madvise(MADV_HUGEPAGE)`），
  避免表自身访问引入额外 dTLB 压力；基址在循环外载入寄存器（§5.2 的 rdx / §5.3 的 x19）；
- 表内容只被慢路径写入（release）、被快路径读取（acquire + 地址依赖），无快路径写共享；
- 大小可调：`VAMAP_IDX_BITS` 编译期可改（默认 20）。8 MiB 意味着单个热表项在 L1 命中、
  全表随机访问通常超出 L2 容量，并在多数 LLC（≥8 MiB）内命中或下探 DRAM——这正是
  test.md 中 L1 命中 / cache miss 两类场景的硬件基础。§5/§6.3 的 C/汇编示例按默认
  20 位写死 `0xFFFFF`，表长按 `2^20` 计算；实际构建应由 `VAMAP_IDX_BITS` 生成表长与掩码，
  改动索引位宽时同步更新汇编立即数。

## 5. 快速路径：查表转换的最优指令实现

### 5.1 C 参考实现（默认，可移植）

```c
extern uint64_t *vamap_table;                 /* 8 MiB 映射表，init 时 mmap */
void *vamap_slow_create_fill(uint64_t vva);   /* 慢路径：创建并填充表项，返回 VA */

static inline __attribute__((always_inline, hot))
void *vva2va(uint64_t vva)
{
    /* 默认 strict：显式掩掉 bit[63:52] 的 tag，满足 readme 的“52 位及以上忽略” */
    uint32_t idx = (uint32_t)(vva >> 32) & 0xFFFFFu;
    uint64_t e = __atomic_load_n(&vamap_table[idx], __ATOMIC_ACQUIRE);
    if (__builtin_expect(e == 0, 0))
        return vamap_slow_create_fill(vva);   /* 无效：调用运行时创建函数并填充表项 */
    return (void *)(e | (uint32_t)vva);       /* 合并：VA 高位 | 零扩展 offset */
}
```

`-O2` 下编译器通常直接生成 5.2 节的 strict 目标序列（x86-64），用 `scripts/check_fastpath.sh`
（objdump 抽查）验证。

### 5.2 x86-64 目标序列

```asm
; 输入：rdi = vva，rdx = vamap_table（表基址，循环内驻留寄存器）
movq  %rdi, %rcx
shrq  $32, %rcx                 ; rcx = vva[63:32]
andl  $0xFFFFF, %ecx            ; idx = vva[51:32]，忽略 tag（strict 默认）
movq  (%rdx,%rcx,8), %rax       ; rax = entry（唯一一次表 load）
testq %rax, %rax
je    .Lslow                    ; entry == 0 → 调 vamap_slow_create_fill
movl  %edi, %ecx                ; ecx = 零扩展后的 offset
orq   %rcx, %rax                ; rax = VA = entry | offset
; 此后以 (%rax) 执行实际 ld/st
```

要点：

- **合并必须用“零扩展 + OR”两步**：`movl %edi, %ecx` 把 offset 零扩展到 64 位，
  `orq %rcx, %rax` 再与表项高位合并。不能用 `movl %edi, %eax` 代替——32 位写会
  同时清零 rax 高 32 位，把表项中的区域基址丢掉；
  寄存器-寄存器 `mov` 在多数微架构上可由重命名阶段消除（move elimination，0 执行端口），
  OR 的关键路径约 1 cycle；
- **test/je 宏融合**为 1 uop；`(%rdx,%rcx,8)` 使用比例变址，无需额外 lea；
- 指令数：**strict 8 条 / 约 6 uops（循环内）**；若构建约定 tag≡0
  （`VAMAP_FAST_TAG0`），删除 `andl` 后为 **7 条 / 约 5 uops**；
  构建系统以宏 `VAMAP_CONV_INSNS`（x86-64 strict 8 / fast 7；ARM64 5）把该数值暴露给测试框架，
  供 `instr_per_op` 断言使用（test.md §9.3）；
- 其中 1 条 `mov` 用于把原 vva 保留给慢路径调用：移位是破坏性操作，而慢路径需要
  完整 vva、合并 offset 又需要低 32 位。若目标平台支持 BMI2，用非破坏性的
  `shrx` 取代 `mov+shr` 即可省去该复制，得到 strict 7 / fast 6 的变体
  （SHRX 无立即数形式，移位量须取自寄存器：循环外预置 `movl $32, %r8d`，
  循环内 `shrx %r8, %rdi, %rcx` 即 rcx = vva>>32、rdi 原样保留）；
  默认基线（无 BMI2）按 8/7 计，测试断言以对应构建变体为准；
- BMI2 的 `bextr` 可用 1 条指令直接取 `VVA[51:32]`，strict 约 6 条；`shrx` 只省去
  复制/移位中的一条，strict 仍约 7 条。合并的零扩展 + OR 仍不能省。
- 若要求 tag 任意取值都不影响结果（readme 的“52 位及以上忽略”），保留 `andl` 作为默认；
  fast 变体必须在分配器或调用约定保证 tag≡0 时使用。

### 5.3 ARM64 目标序列

```asm
; x0 = vva，x19 = vamap_table（表基址）
ubfx x9,  x0, #32, #20          ; idx = VVA[51:32]，天然忽略 tag
add  x10, x19, x9, lsl #3       ; LDAR 不支持比例变址，先算地址
ldar x10, [x10]                 ; acquire load（C11 语义）
cbz  x10, .Lslow
bfi  x10, x0, #0, #32           ; 低 32 位插入 offset，保留表项高位
; 以 [x10] 执行 ld/st
```

要点：

- 不能用 `mov w10, w0` 做合并：32 位写会把 x10 高 32 位清零，丢失表项中的区域基址；
  必须用 `bfi x10, x0, #0, #32`（或 `mov w11, w0` + `orr x10, x10, x11`）保留高位；
- `ubfx` 天然只取 `[51:32]`，因此 strict 与 fast 在 ARM64 上同为 5 条
  （acquire 的 `ldar` 与地址计算各 1 条）；若改用 relaxed `ldr`，可省 `add` 并为 4 条，
  但会失去 C11 acquire 语义。

### 5.4 与替代方案的对比（为什么这是最优形态）

| 方案 | 表项编码 | 合并方式 | 快路径（x86-64） | 评价 |
|------|----------|----------|------------------|------|
| **本设计** | `arena + region<<32`（完整基址），无效 = 0 | 零扩展 + `or` 合并 | mov/shr/and/load/test+je/mov/or = 8 条（~6 uops；fast 去 and 为 7 条） | 1 次表 load，无独立 valid 位；要求 arena 4 GiB 对齐 |
| 只存相对 region | `region<<32` | 先加 arena 基址再 or | +1 条（共 9 条） | 表项少存基址，但快路径多一次 add/lea；arena=0 时与本设计同价 |
| 完整基址但未对齐 | 任意 64 位基址 | `and ~0xFFFFFFFF` + or | +1 条（共 9 条：合并前需清低 32 位） | 不要求 4 GiB 对齐，但合并多一条掩码；对齐后即退化为本设计（直接 or） |
| valid 位放 bit0 | `(arena + region<<32) \| 1` | and+or 掩掉 valid 位 | +1 条（共 9 条：合并前需 `and` 清 bit0） | 判断仍可整字判零（valid 表项恒非零），合并多一条掩码 |
| 通用 C 合并 | 同本设计 | 直接 `e \| (uint32_t)vva`（朴素写法 `(e & ~0xFFFFFFFF) \| (vva & 0xFFFFFFFF)` 多 1 条掩码） | 与本设计同价（编译器生成零扩展 + or） | 可移植回退 |
| 无检查 fault 版 | 同本设计 | 无 test/je | 6 条（strict；fast 5 条）、无分支 | 依赖 SIGSEGV，慢路径成本高，见 §5.7 |

在"必须显式判断有效性、无效时调用运行时函数，且忽略 tag"这一约束下，本形态的
**1 次 vva 复制（慢路径要保留原值）、1 次移位/掩码、1 次表 load、1 次判零分支、
1 次零扩展、1 次 OR 合并**已接近下限；
fast 变体（tag≡0）可省去掩码。表项存完整基址而不是相对 region，避免快路径再承担一次
`arena` 基址加法；代价是 arena 必须 4 GiB 对齐。

### 5.5 开销模型（供 test.md 校验）

- 关键路径：`vva →[shr/and 1–2c]→ 表 load（L1 4–5c；L2 约 +14c；LLC 约 +40c；DRAM 约 +100–200c）
  →[零扩展/OR 1c]→ 实际 ld/st（L1 4–5c）`；
- 表项 L1 命中时，每次访存新增**串行延迟 ≈ 7–8 cycle（strict；fast 6–7）**，分支方向高度可预测；
- 吞吐层面：每次访存多 1 次 load（占用 load 端口）+ 5 条 ALU/分支（shr、and、test、je、or）
  + 2 条寄存器复制 `mov`（可被重命名消除，不占执行端口）→ 对带宽受限的
  STREAM 类内核也会产生可测的吞吐下降，这正是测试要量化的部分；
- 多个独立访存的转换链之间相互独立，乱序执行可跨迭代重叠，摊销部分开销
  （指针追逐类内核无此摊销，转换延迟完整暴露）。

### 5.6 融合访存的汇编宏（示例：32 位 load）

基准内核中使用"转换 + 实际访存"一体化汇编宏，避免内联函数与 ABI 摩擦：

```c
static inline int32_t vload32(uint64_t vva, const uint64_t *tbl)
{
    int32_t v;
    __asm__ volatile(
        "movq  %1, %%rcx             \n\t"
        "shrq  $32, %%rcx            \n\t"
        "andl  $0xFFFFF, %%ecx       \n\t"   /* idx，忽略 tag */
        "movq  (%2,%%rcx,8), %%rax   \n\t"
        "testq %%rax, %%rax          \n\t"
        "jne   1f                    \n\t"   /* 有效：跳到合并；无效：落入慢路径 */
        "movq  %1, %%rdi             \n\t"
        "call  vamap_slow_create_fill\n\t"   /* rdi = vva 入参；返回 rax = VA */
        "jmp   2f                    \n\t"   /* 冷路径：跳过合并，直接访存 */
        "1: movl  %k1, %%ecx         \n\t"   /* ecx = 零扩展 offset */
        "orq   %%rcx, %%rax          \n\t"   /* rax = VA */
        "2: movl  (%%rax), %k0       \n\t"   /* 实际 load */
        : "=&r"(v)
        : "r"(vva), "r"(tbl)
        : "rax", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "cc", "memory");
    return v;
}
```

> 慢路径 `call` 会破坏 ABI 规定的 caller-saved 寄存器；上述 clobber 列表已覆盖全部
> caller-saved 通用寄存器（rax/rcx/rdx/rsi/rdi/r8–r11），因此输入操作数会被分配到
> callee-saved 寄存器，`call` 之后仍有效。若内核中同时有活跃的浮点/向量寄存器，
> 还需按 ABI 追加 `xmm0–xmm15`（或直接使用 C 版 `vload32`）。合并同样必须
> “零扩展 + OR”，不能只写 `%eax`。该布局把慢路径块放在分支与合并块之间，
> 快路径只执行 `jne` 一跳、不额外执行跳过慢路径块的 `jmp`，故快路径增量仍为
> §5.2 的 8 条；调用点还须保持 ABI 要求的 16 字节栈对齐，若内核布局使这一点
> 不确定，应改用 C 版 `vload32`。另外，编译器不知道 asm 内含 `call`，可能仍按
> leaf function 使用 128 B red zone；基准构建须加 `-mno-red-zone`，或把慢路径调用
> 移到独立的非内联函数，避免 callee 覆盖 red zone 中的 spill。

ARM64 版本同构（`ubfx/add/ldar/cbz/bfi` + `bl vamap_slow_create_fill`）。
64 位元素把末尾的 `movl` 改为 `movq`；store 版把 64 位值操作数写入 `(%%rax)`，
转换序列与指令数不变。

### 5.7 变体：无检查 fault 版（备选，非默认）

表项无效时转换结果落在低 4 GiB（entry 0 与 offset 合并），该区域通常未映射，访存触发 SIGSEGV；
在信号处理器中调用创建函数后重执行指令。快路径减为 6 条（strict；fast 5 条）且无分支，
但慢路径成本变为一次信号往返（µs 级），且难以区分空指针 bug。实现时必须把低 4 GiB
预留为 `PROT_NONE`（或保证进程不映射该区间），否则 offset 落入已映射页会静默读错；
该变体也不适用于 offset 合法指向低地址的负载。仅作为对照组实现（`VAMAP_MODE_FAULT`），
不作为默认。

## 6. Mock 运行时库

### 6.1 公共 API

```c
typedef uint64_t vva_t;            /* 52 位 VVA；默认实现忽略 bit[63:52] */

typedef struct {
    uint64_t create_latency_ns;   /* 映射建立时延；测试取 10us/100us/1ms/10ms */
    int      latency_mode;        /* VAMAP_LATENCY_SPIN(默认) / VAMAP_LATENCY_SLEEP */
    uint32_t lock_stripes;        /* 慢路径锁条带数，默认 4096（2 的幂） */
    uint32_t alias_pool_size;     /* bench alias 模式：预留 [1, alias_pool_size] 个 region 供 alias/create 复用；0=禁用（默认），须 < REGION_COUNT */
    int      strict_48bit;        /* 置位时区域号限制在 [1, 2^16-1]（48 位 VA 兼容），并叠加 §2.3 的 arena 上界与 REGION_COUNT-1 */
} vamap_cfg;

int      vamap_init(const vamap_cfg *cfg);
void     vamap_shutdown(void);     /* 释放映射表与 arena；进程退出前可省略 */

vva_t    vamap_vva_alloc(uint32_t nidx);  /* 预留 n 个连续索引（不建映射），返回基址 VVA (idx<<32) */
void     vamap_vva_free(vva_t base, uint32_t nidx);

int      vamap_map_create(uint32_t idx);  /* 0=成功/已存在；非0=失败；仅实际新建时含模拟时延 */
int      vamap_map_destroy(uint32_t idx); /* 表项置 0；引用计数归零时回收区域号（无模拟时延） */
int      vamap_map_alias(uint32_t idx, uint32_t region);
                                          /* 测试用：预填到已有 region（引用计数 +1），不计时延 */

void     vamap_set_create_latency_ns(uint64_t ns);
void    *vamap_pin(uint64_t vva);         /* 返回当前 4GiB 窗口真实基址（无效时先创建；供循环提升查表） */

typedef struct {
    uint64_t creates, destroys;           /* 慢路径事件计数（不含 setup 预填；预热前清零；校验用） */
    uint64_t spin_ns;                     /* 累计模拟时延（仅 create 的 latency 部分） */
    uint64_t slow_ns_total;               /* 全局累计（多线程求和；含锁、分配、模拟时延） */
    uint64_t fast_hits, slow_calls;
} vamap_stats;
vamap_stats vamap_get_stats(void);
```

> 计数实现：`creates/destroys/fast_hits/slow_calls/spin_ns/slow_ns_total` 在多线程下用
> 原子增量（或每线程累加后汇总），`vamap_get_stats` 返回读取时刻的快照；否则 T11/C5
> 的计数校验会因丢失更新而失败。

### 6.2 地址空间预留

`vamap_init` 一次性完成：

- 按 §4 用匿名 `mmap` 分配 `2^VAMAP_IDX_BITS` 个 `uint64_t` 的映射表（默认 8 MiB），
  清零并做 2 MiB 对齐/大页建议，基址写入 `vamap_table`；
- `mmap(NULL, ((size_t)REGION_COUNT << 32) + (4ULL << 30), PROT_READ|PROT_WRITE,
  MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)`
  预留 `REGION_COUNT × 4 GiB` **虚拟**空间作为真实 VA 竞技场（`MAP_NORESERVE` 下仅占虚拟地址）。
  竞技场必须 **4 GiB 对齐**：多预留 4 GiB 后按对齐地址裁剪，或用 `MAP_FIXED_NOREPLACE`
  在 4 GiB 对齐的空洞处预留；否则表项低 32 位非零，快路径合并需额外掩码。
  满配 `2^17` 个区域 = 512 TiB，连同 4 GiB 对齐余量共需预留 512 TiB + 4 GiB，故要求
  平台用户 VA 至少 512 TiB + 4 GiB 并为进程自身映射留出余量（x86-64 5 级页表 LA57、
  ARM64 52 位 VA 等远大于此）；`REGION_COUNT` 按平台用户 VA 上限自高向低按 2 的幂回退，
  取首个可成功预留并为进程自身映射留出余量的档位，并在测试元数据中记录实际值。
  下表是**理论最大值**（假设 arena 位于最低可用地址）；实际值还受 arena 基址与进程
  已有映射影响，可能低于表中值：

  | 平台（用户 VA 上限） | 最大 `REGION_COUNT` | 可用 region 数上限 | 竞技场 |
  |----------------------|---------------------|--------------------|--------|
  | x86-64 5 级（LA57，用户 64 PiB）/ ARM64 52 位 VA（用户 4 PiB） | `2^17` | `2^17−1` | 512 TiB |
  | ARM64 48 位 VA（用户 `2^48` = 256 TiB） | `2^15` | `2^15−1` | 128 TiB |
  | x86-64 4 级（用户空间 `2^47` = 128 TiB，canonical） | `2^14` | `2^14−1` | 64 TiB |

  说明：x86-64 4 级页表用户空间只有 128 TiB，`2^15` 档的 128 TiB 竞技场已占满全部
  用户地址；ARM64 48 位 VA 的用户空间恰为 256 TiB，`2^16` 档（256 TiB 竞技场）同样
  占满全部用户地址。故两平台的回退终点分别为 `2^14` 与 `2^15`，而非 `2^16/2^15`。
  严格 48 位模式还要求 `arena + (最大已分配 region 号 + 1)×4 GiB ≤ 2^48`，即
  `region 号上界 ≤ floor((2^48 − arena)/4 GiB) − 1`；再叠加 `2^16−1` 的分配上限。
  因此严格模式可用的 region 号上界为
  `min(2^16−1, REGION_COUNT−1, floor((2^48 − arena)/4 GiB) − 1)`，仅当 arena 足够低时
  才可能达到 `2^16−1`；若释放后产生空洞，仍以已分配的最大 region 号为准。若
  `arena ≥ 2^48`，严格模式无法建立任何映射，`vamap_init` 应返回错误（而不是让差值
  无符号回绕）；
- 可用 region 数（上限 `2^17−1`，平台相关，见上表；严格 48 位模式再叠加 `2^16−1` 分配上限）
  恒小于表项数 `2^20`；需要“全表有效”时测试框架用 `vamap_map_alias` 预填到少量 region，
  正常创建在 region 耗尽时返回错误；
- **bench 专用 region 池**（`alias` 模式）：测试框架设 `cfg.alias_pool_size = 64`，
  `vamap_init` 预留 region 号 `[1, 64]` 作为固定池（引用计数）；普通模式下
  `region_alloc` 跳过该区间，alias 模式下 `vamap_map_create` 改为从池中按轮转/引用计数取用
  （不消耗池外新 region）；`vamap_map_alias(idx, region)` 把大量表项预填到池内 region，
  从而支持在 `2^20` 表项上测非零无效比例而不耗尽 region 号；
  create 仍正常计数并执行时延模拟。该池复用模式中任意数据别名只用于性能测量；
  alias API 与引用计数由 C1/C7 在受控池上校验，C2 走非 alias 路径（test.md §4.2）；
  `region` 参数即池内号，测试框架按轮转/确定性规则选择；正常 init 默认
  `alias_pool_size = 0`（禁用）；
- region `r` 的真实基址 = `arena + ((uint64_t)r << 32)`；
- 分配区域号空闲链（region 0 保留）、初始化条带锁、校准 TSC。

由此**默认 SPIN 模式下创建路径不含任何系统调用**：create = 取区域号 + 忙等模拟时延 +
原子发布，时延模拟干净、可测；SLEEP 模式则用 `clock_nanosleep` 显式让出核心。
物理页在首次写时由内核按需提交；基准在预热阶段预先触碰
将要访问的有效 offset，避免把缺页开销混入快路径测量。初始无效表项在创建后的首次
数据访问仍可能触发缺页，测试框架将其单列（test.md §9.2），不与配置的
`create_latency_ns` 混淆。

### 6.3 慢路径：映射创建与表项填充

```c
void *vamap_slow_create_fill(uint64_t vva)
{
    uint32_t idx = (uint32_t)(vva >> 32) & 0xFFFFFu;
    lock_t  *lk  = &locks[idx & (cfg.lock_stripes - 1)];

    lock(lk);                                                    /* 条带自旋锁 */
    uint64_t e = atomic_load_explicit(&vamap_table[idx], memory_order_acquire);
    if (e == 0) {                                                /* double-check：持锁复查 */
        uint32_t region = region_alloc();                        /* 全局自旋锁取区域号（快） */
        if (region == 0) { unlock(lk); return NULL; }            /* region 号耗尽 */
        spin_until(tsc_deadline(cfg.create_latency_ns));         /* 模拟建立时延（SPIN；SLEEP 走 clock_nanosleep，见下） */
        e = (uint64_t)arena + ((uint64_t)region << 32);
        atomic_store_explicit(&vamap_table[idx], e, memory_order_release);  /* 原子发布 */
    }
    unlock(lk);
    return (void *)(e | (uint32_t)vva);
}
```

要点：

1. **double-check**：持锁后复查表项，保证同索引并发时只创建一次、只分配一个区域号；
2. **发布原子且有序**：release store 单条原子写完成"填充表项"；运行时若在发布前
   写入了区域元数据/初始化数据，release/acquire 保证这些写对后续读者可见。本 mock
   只发布区域基址，不初始化 4 GiB 区域，创建后的普通数据写仍需应用同步；
3. **同索引并发语义**：其余线程在条带锁上排队，持锁者填完后它们复查命中、直接返回
   ——与真实缺页处理"一个线程填页、其余等待"的串行化语义一致；
4. **时延模拟两种模式**：
   - `VAMAP_LATENCY_SPIN`（默认）：`init` 时用 `CLOCK_MONOTONIC` 标定 TSC 频率，
     `spin_until` 用 `rdtsc + pause` 忙等到 deadline。占核、CPU 开销确定性强，
     适合测量"分配代价直接计入线程时间"的场景；
   - `VAMAP_LATENCY_SLEEP`：`clock_nanosleep(绝对 deadline)`，让出核心，
     模拟系统调用/IO 型分配器；墙钟时间包含调度延迟，CPU 占用不计入。
     两种模式的语义差异在 test.md §4.3 说明，默认 SPIN。**SLEEP 模式下条带锁
     必须改用可睡眠互斥锁（或等价的释放–重取机制），不得持有自旋锁睡眠**；
     SPIN 模式才使用 §6.3 代码所示的自旋锁。
5. **region 号耗尽**：`region_alloc()` 返回 0 时创建失败（返回 `NULL`），不发布表项；
   benchmark 通过 `vamap_map_alias` 预填/池复用或缩小表项空间避免（test.md §4.2/§4.4）。
   透明接口 `vva2va` 没有错误返回通道，耗尽时会把 `NULL` 交给调用点，后续解引用将崩溃；
   应用须保证活跃映射数不超过可用 region 数，或改用可报告错误的显式创建接口。

### 6.4 映射销毁

```c
vamap_map_destroy(idx):
    lock(stripe);
    uint64_t e = atomic_load_explicit(&vamap_table[idx], memory_order_acquire);
    atomic_store_explicit(&vamap_table[idx], 0, memory_order_release);
    if (e) {
        uint32_t r = (uint32_t)((e - (uint64_t)arena) >> 32);
        if (region_unref(r) == 0) region_free(r);
    }
    unlock(stripe);
```

- 销毁**不含模拟时延**（readme 只要求建立时延可配）；
- **alias 共享 region**：多个 idx 可经 `vamap_map_alias` 指向同一 region，因此区域号需
  维护引用计数；只有最后一个引用销毁时才回收，否则单个 alias 表项销毁会误回收其他
  idx 仍在使用的区域；
- **竞态约束**：销毁与该映射的并发使用者之间需外部同步。mock 中区域内存映射在进程
  生命周期内保留，不会 use-after-free；但区域号回收复用后，在途的旧转换结果
  可能指向新映射的内存。测试框架的 churn 场景遵守"阶段边界销毁"或
  "线程私有索引"纪律（test.md T12），真实系统可扩展引用计数/epoch 回收。

### 6.5 区域号分配器

全局自旋锁 + 空闲链 + 每 region 引用计数（alias 共享用）；create 的耗时主体是时延模拟，
分配器锁不在关键争用点上（全局自旋锁只在取号瞬间持有；条带锁覆盖整个创建过程——
不同条带上的索引并行忙等，同条带索引与同索引一样在条带锁上串行，含时延等待）。
锁顺序统一为**条带锁 → 全局自旋锁**；全局锁临界区只操作空闲链/引用计数，不回调
创建/销毁路径，避免反向获取造成死锁。

## 7. 线程安全设计

| 共享对象 | 快路径 | 慢路径 | 保障机制 |
|----------|--------|--------|----------|
| 表项 `vamap_table[idx]` | 无锁读 | 互斥写 | acquire load + **地址依赖**；release store 发布 |
| 区域号空闲链 / 引用计数 | 不触及 | 全局自旋锁 | 自旋锁（不在时延等待内） |
| 条带锁数组 | 不触及 | 取锁 | 每条目 64B padding，避免伪共享 |
| 区域内存 | 经 VA 访问 | create 前写或应用同步 | release/acquire 顺序 |

内存序论证：

- 快路径使用 `atomic_load(..., ACQUIRE)`：与发布端 release store 形成 C11
  synchronizes-with，保证表项可见时其先行写入的数据也可见；x86-64 上 acquire load
  就是普通 `mov`，无额外指令；ARM64 上为 `ldar`（LDAR 不支持比例变址，需额外一条
  `add` 计算地址）；
- 表项值作为最终访存的基址，形成硬件级地址/数据依赖，进一步阻止后续数据访问早于
  表项 load 完成；仅依赖 relaxed + 地址依赖在 x86/ARM64 上通常可行，但不满足 C11 的
  happens-before，不能作为可移植实现；
- 发布端 release store 保证"先行数据写入 → 表项可见"的全局顺序；
- 上述语义在正确性套件中以 TSAN 验证（test.md C4/C5）。

## 8. 应用侧集成

```c
/* 每次访存前的内联转换宏 */
#define VLOAD(type, p)     (*(type *)(uintptr_t)vva2va((uint64_t)(uintptr_t)(p)))
#define VSTORE(type, p, x) (*(type *)(uintptr_t)vva2va((uint64_t)(uintptr_t)(p)) = (x))

/* STREAM Triad 示例 */
VSTORE(double, A + i, VLOAD(double, B + i) + q * VLOAD(double, C + i));
```

上述 C 宏便于阅读与移植；基准中对延迟敏感的路径改用 §5.6 的 volatile 融合汇编宏，
确保每次 ld/st 各执行一次转换、不被编译器 CSE 或提升到循环外。

- **对 VVA 的指针算术无需转换**：idx/offset 分段使 `base + i*8` 在 4 GiB 内自然成立；
  跨越 4 GiB 边界时索引 +1，前提是目标索引已建立映射（`vamap_vva_alloc(n)` 支持一次
  分配连续 n 个索引供巨数组使用）；
- `vamap_pin(vva)` 供流式循环手动提升查表（转换成本上界对照，见 test.md T15）；
- 编译建议：`-O2 -fno-semantic-interposition`；平台宏选择 x86-64 / ARM64 / 通用 C 回退；
- 快路径形态校验：`scripts/check_fastpath.sh` 反汇编抽查生成的指令序列是否与 §5.2 一致。

## 9. 正确性保障

正确性套件（转换位精确性、创建/销毁往返、同索引竞争、TSAN 并发、时延精度、
churn 压力、alias 引用计数）在 test.md §5 定义，性能测试前必须全部通过作为门槛。

## 10. 已知限制与扩展方向

- 表项编码假定区域基址 4 GiB 对齐（offset 32 位），不支持任意对齐的对象放置；
- destroy 与在途使用的竞态通过使用纪律（阶段边界销毁/线程私有索引）约束，
  未实现 epoch/hazard-pointer 回收；region 引用计数仅用于 alias 共享（扩展项：epoch 回收）；
- 未做两级表（8 MiB 单表对 LLC 友好；若缩小索引位宽可重审）；
- fault 版（§5.7）与 per-core 表、批量预转换、编译器 hoisting 属后续优化空间，
  均以 test.md T15 的方式量化对比。
