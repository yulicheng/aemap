# test.md — 测试框架与测试用例设计(x86-64 / AArch64 共享)

> 依据 `readme.md`:设计测试框架,量化软件 VVA→VA 转换(见 `design.md`)在每次 `ld/st` 上引入的 overhead;测试用例必须覆盖"重复访问相近 VVA 使映射表 L1 命中"与"随机访问 VVA 使映射表 cache miss",并补充常见软件访存 pattern。
> 框架原则:**一套框架、一套用例、一份内核代码**,两架构仅转换层(`arch/*/vva_impl.h`)不同,结果可直接横向对比。

---

## 1. 目标与度量定义

### 1.1 overhead 定义

对同一访存序列,分别以 **direct**(裸指针,无转换)与 **conv**(经 `vva2va` 内联转换)执行:

```
Δcycles/op = (cycles_conv − cycles_direct) / n_ops     # 核心指标:每访存净增周期
Δins/op    = (inst_conv − inst_direct)   / n_ops     # 指令增量,须与 design.md §8.1 精确一致
Δl1m/op    = (l1_miss_conv − l1_miss_direct) / n_ops # 其中表访问贡献的 L1 miss
Δdtlb/op   = (dtlb_conv − dtlb_direct)     / n_ops
```

公平性保证(框架强制):
- **同一条访存序列**:direct 与 conv 访问**完全相同的物理 cache line**(VA 列表由同一转换函数预计算,见 §3.5);
- **同一编译单元、同一优化等级**,内核代码逐字相同,仅 `A2P` 宏一层之差(§3.8);
- 转换层只增加"查表 + ALU 合并",不改变数据访问本身。

### 1.2 指标集合

cycles/op、ins/op、IPC、L1/L2/LLC miss per op、dTLB miss per op、带宽(GB/s,C8)、MAD/相对噪声。所有指标 direct/conv 成对出现。

---

## 2. 框架总体结构(一套框架,两架构)

```
                 ┌────────────────────────────────────────────┐
   一套内核       │  bench/kernels.c  (只用 VVA_LD/VVA_ST/A2P) │   ← 架构无关,只编译一份源码
                 └──────────────┬─────────────────────────────┘
                                │ 编译期宏
   一套框架                      ▼
                 ┌────────────────────────────────────────────┐
                 │  bench/harness.c  计时/PMU/缓存控制/统计     │   ← 架构相关计时封装内部实现
                 │  bench/patterns.c 地址序列生成器             │
                 │  bench/main.c     CLI                       │
                 └──────────────┬─────────────────────────────┘
                                │ include/vva.h 派发
        ┌───────────────────────┴───────────────────────┐
        ▼                                               ▼
 arch/x86_64/vva_impl.h(B8,§design.3)      arch/aarch64/vva_impl.h(A4,§design.4)
```

### 2.1 目录结构

```
bench/
├── main.c          # CLI:--case --k --enc --tag --base --reps --csv
├── harness.h/.c    # 计时封装、perf_event_open、rdpmc mmap、缓存/TLB 控制、统计
├── patterns.c      # seq/rand/stride/chase/zipf/gather 地址序列生成(xorshift 预生成)
├── kernels.c       # 内核 + 双实例实例化(§3.8)
├── kernel_defs.h   # 内核定义(被实例化两次)
├── run_all.sh      # 环境设置 + 全用例扫描 + CSV 落盘
└── analyze.py      # 汇总:Δ 曲线、变体排名、架构对比表
```

### 2.2 CLI 与一次运行

```bash
sudo sysctl kernel.perf_event_paranoid=1        # 允许用户态计数
sudo cpupower frequency-set -g performance      # 锁频
./vva_bench --case c4 --k 4096 --enc B8 --tag strict --base pinned --reps 30 --csv out.csv
taskset -c 2 ./vva_bench ...                    # 固定核(建议配合 isolcpus / 关 SMT 兄弟线程)
```

所有用例、所有变体、两架构由 `run_all.sh` 驱动,产出统一 CSV(§6)。

---

## 3. 测量基础设施

### 3.1 计时封装

| 架构 | 首选 | 说明 |
|---|---|---|
| x86-64 | **PMU cycles**(rdpmc mmap) | `lfence;rdtsc` 墙钟仅作交叉校验 |
| AArch64 | **PMU CPU_CYCLES**(perf mmap 用户态读) | ⚠ `cntvct_el0` 是固定频率架构定时器,**不是** CPU cycles,只作墙钟回退 |

内核单次运行 ~ms 量级,经 perf_event_open + mmap 页用户态直接读计数器,零 ioctl 开销。

### 3.2 PMU 事件表

以 perf 通用事件为跨架构基线,raw 事件做细化(以各核 TRM / `perf list` 为准):

| 指标 | perf 通用事件 | x86-64 细化(Intel) | AArch64 细化(PMUv3) |
|---|---|---|---|
| cycles / inst | `cycles` / `instructions` | 固定计数器 | 0x11 / 0x08 |
| L1D miss(load) | `L1-dcache-load-misses` | `MEM_LOAD_RETIRED.L1_MISS` | 0x03 L1D_CACHE_REFILL |
| L2 miss | — | `MEM_LOAD_RETIRED.L2_MISS` | 0x17 L2D_CACHE_REFILL |
| LLC miss | `LLC-load-misses` | `LONGEST_LAT_CACHE.MISS` | 实现相关 |
| dTLB miss | `dTLB-load-misses` | `DTLB_LOAD_MISSES.WALK_ACTIVE` | 0x05 L1D_TLB_REFILL(实现相关) |

**表访问归因**:conv 与 direct 之差中的 miss 即表访问贡献(数据访问完全一致,§1.1)。

### 3.3 环境控制清单

- 固定核 + 隔离(`taskset`/isolcpus),关 SMT 兄弟或错核绑定;
- governor=performance,记录测试期间实测频率;
- 数据区 `MAP_POPULATE` + `mlock` 预触;THP 显式开关(`madvise(MADV_HUGEPAGE)` / `never`)按用例控制;
- 可选:BIOS/MSR 关硬件预取器(随机 pattern 下预取器基本无效,默认不关,备注即可);
- NUMA 单节点绑定。

### 3.4 缓存与 TLB 状态控制(用例的"表状态"由这三个原语组合)

| 原语 | 实现 | 用途 |
|---|---|---|
| `table_warm(K)` | 按访问序列触碰前 K 项(或随机 K 项) | 表热集精确控制 |
| `table_flush()` | x86:`clflush` 表已用区间;AArch64:`dc civac`+`dsb`(SCTLR.UCI 使能,Linux 默认开放) | 表全冷 |
| `evict_llc()` | 流式清扫 3×LLC 容量缓冲(两架构通用回退) | 无 clflush 指令时的全冷 |
| `data_state(warm/cold)` | 同上原语作用于数据区 | 数据侧状态控制 |

### 3.5 负载生成与地址解耦

- 地址序列**预生成**(`xorshift64*`,固定种子):`addr[i] = (idx<<32) | off`;测量期间零 RNG 开销;
- 数据区为 `mmap(MAP_NORESERVE)` 的稀疏 VA 区,实际触碰页数由用例控制;
- **关键解耦技巧**:映射采用**多对一**(所有 idx → 同一 H)时,数据工作集恒定(如 4KB),而表工作集随 K 增长——用于把"表 cache 行为"与"数据 cache 行为"分离(C4);端到端场景(C4b)用 idx→独立 H。

### 3.6 统计协议

- 每配置:2 次预热 + **30 次重复**;
- direct/conv **ABBA 交错**执行(`D,C,C,D ×15`)成对做差,抵消频率/热漂移;
- 报告中位数 ± MAD;MAD/median > 5% 判为不稳定,自动加测;
- 变体间比较用同进程顺序运行,消除进程间差异。

### 3.7 防 DCE 与编译器干扰

- 结果异或累积进 `volatile uint64_t g_sink`;
- 序列基址:`asm volatile("" : "+r"(p))` 防指针提升;
- 内核 `-O2`,不使用 `-fno-omit-frame-pointer` 以外的额外降噪选项;codegen 随抽样打印(objdump)存档,`Δins/op` 与 design.md §8.1 硬校验。

### 3.8 内核双实例机制(共享内核的关键)

```c
/* bench/kernel_defs.h —— 内核只写一遍 */
static void K_seq_read(const uint64_t *addr, size_t n, uint64_t *sink) {
    uint32_t acc = 0;
    for (size_t i = 0; i < n; i++)
        acc ^= *(const uint32_t *)A2P(addr[i]);      /* 唯一的差异层 */
    *sink += acc;
}

/* bench/kernels.c —— 实例化两份 */
#define CONV 0
#include "kernel_defs.h"      /* A2P(a) ≡ (void*)(uintptr_t)(a):列表内已是真实 VA */
#define CONV 1
#include "kernel_defs.h"      /* A2P(a) ≡ vva2va(a):每次 ld/st 内联转换 */
```

> `pinned` 构建按 design.md §3.1/§4.1 实现(保留寄存器 + 字面引用);无保留寄存器的编译器(如 x86-64 上的 clang)自动回退到 `mat` 物化形式,框架按构建变体记录(§7)。

---

## 4. 测试用例

### 4.0 用例总表

| # | 名称 | 访存模式 | 表状态 | 数据状态 | 扫描参数 | 核心指标 | 对应 readme |
|---|---|---|---|---|---|---|---|
| C0 | 差分正确性(gate) | 随机 ld/st + tag 扰动 | 任意 | — | — | 全部断言通过 | 正确性前提 |
| C1 | 顺序流 | seq 读/写,64B~64MB | 1 项热 | 递增 | 流长 | Δc/op,带宽 | 补充 |
| **C2** | 相近 VVA 热点 | 8~64B 窗口反复 | **1~8 项热(L1)** | 热 | 窗口宽 | Δc/op 最优下界 | **必测** |
| C3 | 段内随机 | idx 固定,offset 随机 | 1~8 项热 | 热随机 | 段大小 | Δc/op | 补充 |
| **C4** | 随机索引扫描 | idx 均匀随机 | **K 项(K 扫描)** | 见 §3.5 | K=8…2^20 | Δc/op(K) 阶梯曲线 | **必测** |
| C5 | 表全冷 | C2/C4 模式 | **每次全 flush** | 热 | — | Δ≈+1 次 DRAM 延迟 | 必测延伸 |
| C6 | 指针追逐 | 依赖加载链 | 热 / 冷两档 | 热 | 链长 | Δ延迟/hop | 补充 |
| C7 | 跨段步长 | idx+1,off 固定 | 表沿线顺序流 | 顺序页 | stride | Δc/op(表预取器作用) | 补充 |
| C8 | 拷贝带宽 | memcpy,块级转换 | 两档 | 热 | 块 64B~128B | GB/s,Δ% | 补充 |
| C9 | gather/scatter | idx+off 双随机稀疏 | K 档 | 稀疏 | K | Δc/op | 补充 |
| C10 | Zipf 热冷混合 | idx~Zipf(0.9) | 混合 | — | K | Δc/op(真实分布) | 补充 |
| C11 | 读写混合 | R / RMW / W 三档 | 热 | 热 | 读写比 | ld vs st 的 Δ 对称性 | 补充 |
| C12 | TLB 交互 | C1/C4 复测 | 两档 | 大足迹 | 4KB vs 2MB 页 | Δdtlb/op | 补充 |
| C13 | 变体矩阵 | 在 C2/C4 上跑全部变体 | 两档 | — | 变体 × 模式 | 变体排名表 | design.md §6 |
| C14 | 多流计算内核 | daxpy / matmul 内层(2~3 条流 + 复用) | 热/混合 | 热 | 矩阵规模、块大小 | Δc/op、ALU 空隙隐藏率 | 补充(计算型 pattern) |

### 4.1 C0 差分正确性(性能测试前置 gate,不通过则中止)

```text
1) 随机 10^6 次 LD/ST:conv 读值 ≡ golden 数组值;写后读回一致
2) tag 不变性:strict 模式下 vva 与 (vva | tag<<52) 转换结果逐位相同(tag ∈ 随机 12 bit)
3) fast 模式:tag≡0 契约下同样通过(并验证契约注释存在)
4) entry0 哨兵、W 上限断言、对齐(转换后指针满足原指针对齐)检查
5) 公平性:direct 与 conv 的地址列表逐元素相等(§1.1 强制条件,程序化断言)
```

> x86-64 上 strict/fast 序列的语义已在开发机完成 10^6 随机(tag 扰动)验证(design.md 附录 A);C0 在目标机上再对两架构做最终把关。

### 4.2 C1 顺序流

64B→64MB 流式读/写(off 单调递增,基本不跨段)。**预期**:Δ ≈ 吞吐型下界 1~3 c/op;表项常驻 L1,几乎零 miss 增量。用于确认"额外指令被乱序隐藏"的程度。

### 4.3 C2 相近 VVA 热点(必测,最优下界)

访问集中在 1~4 个 idx 的 8~64B 窗口内,表项同一条 cache line,永远 L1 命中。**预期**:Δc/op ≈ 1~3(吞吐)/ ≤8~10(窗口内地址相关时);`Δins/op` 必须精确等于序列指令数(x86 strict=5,ARM strict=4)。该用例给出本方案**不可逾越的下界**,直接检验指令序列质量。

### 4.4 C3 段内随机

idx 固定、offset 均匀随机(数据随机、表恒热)。与 C2 之差即"数据随机性本身"的影响,确认表热时转换开销与数据 pattern 无关。

### 4.5 C4 随机索引扫描(必测,表 miss 曲线)

`idx = rng()%K`,`off = rng()&0xFF8`。默认多对一映射解耦数据集(§3.5);K 扫描:

```
K ∈ {8, 64, 256, 1024, 4096, 16384, 65536, 262144, 1048576}
表足迹(B8): 64B→8MB(A4 编码为 32B→4MB),依次穿越 L1(≈32-48KB)/ L2(≈0.5-1MB+)/ L3(≈数 MB+)/ DRAM
```

**预期**:Δc/op(K) 呈阶梯上升:热段平坦(+1~3),越过 L1 容量后跳升(+10~20,表 L2/L3),DRAM 段 +100 以上。C4b(独立 H)叠加数据 miss,演示 design.md §8.2 的**串行叠加效应**(表 miss 延迟 + 数据 miss 延迟)。此曲线是本项目的核心产出。

### 4.6 C5 表全冷

每轮重复前 `table_flush()`(x86 clflush / ARM dc civac / 回退 evict_llc)。**预期**:Δ ≈ 一次表线 DRAM 访问延迟(~120-250c),且 ≈ C4 曲线的 DRAM 段值,两法互为校验。

### 4.7 C6 指针追逐(延迟敏感场景)

链表 next 为 VVA,依赖加载 N 跳(热表:常数 idx;冷表:随机 idx+flush)。**预期**:热表 Δ ≈ 完整转换关键路径(x86≈8c,ARM≈6c);冷表 Δ ≈ 表延迟 + 数据延迟(串行),放大效应显著——量化"转换在延迟受限负载下的最坏代价"。

### 4.8 C7 跨段步长

off 固定,idx 每次 +1:表访问呈**顺序流**(16 项/line,A4),数据页顺序递增。**预期**:硬件预取器部分隐藏表 miss,Δ 介于 C2 与 C4 同足迹值之间;若 A4/B8 差异可见(每 line 项数不同),在此用例显现。

### 4.9 C8 拷贝带宽

memcpy 型内核(源、目的均为 VVA 指针),**每 64/128B 块转换一次指针**(而非每字),测 GB/s 与 Δ%。加测"每字转换"上界变体,以及非临时写(NT store)变体(验证转换开销能否与写路径重叠)。**预期**:块级转换摊薄后 Δ% < 5%(热表);给出真实应用可接受的量级参考。

### 4.10 C9 gather/scatter

idx 与 offset 双随机稀疏(稀疏度扫描)。观察稀疏模式下表/数据两级 miss 叠加与 MLP 部分遮蔽。

### 4.11 C10 Zipf 热冷混合

idx~Zipf(s=0.9) 覆盖 K 项:少量热段 + 大量冷段,贴近真实负载。**预期**:Δ 介于 C2 与 C4 之间,且对 K 的尾部不敏感——检验"热集假设"在真实分布下的成立程度。

### 4.12 C11 读写混合

R:1:0、RMW:1:1、W:0:1 三档(热表)。转换序列对 ld/st 相同,**预期** Δ 对称;若不对称,定位 store 路径端口/缓冲效应。

### 4.13 C12 TLB 交互

C1/C4 在 4KB vs 2MB 页、表自身 4KB vs 2MB 映射下复测,记录 Δdtlb/op。**预期**:2MB 数据页消除数据侧 dTLB 噪声;全表 1M 项(B8=8MB)在 4KB 页下占 2048 页,超过 L2 TLB 时表访问自身产生 dTLB miss——论证"表用 hugepage 映射"的必要性。

### 4.14 C13 变体矩阵

在 C2(热)与 C4(K=4K/64K/1M)上运行 design.md §6 全部变体,输出排名表:

| 变体 | 说明 |
|---|---|
| 默认(strict+pinned,默认编码) | 基准 |
| fast / enc 互换 / bextr(v3) / mat 基址 | 指令序列级 |
| pf 表项预取 / cache 段缓存 / gs GS-base(x86) | 算法级 |
| 纯 C 版 | 编译器对照 |

**验收问题**:手写序列是否稳定优于纯 C 版?BMI2 在哪些微架构上赢?GS-base 在段粘性负载下能到多少?——均由本矩阵回答。注意 `pinned` 在 x86-64 clang 下不可用(design.md 附录 A),排名表按编译器分栏。

### 4.15 C14 多流计算内核(计算型 pattern)

- **daxpy**:`y[i] = a*x[i] + y[i]`,2 条数据流(不同 idx,2 个表项常驻);内存受限但 ALU 有余量;
- **matmul 内层**:`C[i][j] += A[i][k]*B[k][j]`,3 条流 + 复用,典型 ALU 受限、访存间隙大;
- 分块/非分块两档,矩阵规模跨越 L2/L3 各一档。

**预期**:转换指令主要被塞进 ALU/发射空隙 → **Δc/op 接近 0**;表项多但稳定常驻,无 miss 增量。该用例给出"计算密集场景转换几乎免费"的上界证据,与 C1/C8(访存受限)形成对照。验证方式同 §3.8 双实例,仅改变数据布局的 idx 分配。

---

## 5. 输出与分析

### 5.1 CSV schema

```csv
arch,variant,enc,tag,base,isa,case,param,rep,
  direct_cyc,conv_cyc,n_ops, direct_ins,conv_ins,
  direct_l1m,conv_l1m, direct_l2m,conv_l2m, direct_llcm,conv_llcm,
  direct_dtlb,conv_dtlb, freq_mhz
```

### 5.2 analyze.py 产出

1. **C4/C5 K-Δ 阶梯曲线**(核心图,x86 与 ARM 同图双线);
2. C2 下界表 + `Δins/op` 与 design.md §8.1 的一致性校验(自动 PASS/FAIL);
3. C13 变体排名表(热/冷两栏);
4. 架构对比表:同用例 Δ、IPC、每访存 miss 差异;
5. 噪声报告(MAD>5% 的配置清单)。

---

## 6. 验收标准(预期阈值,标注"待实测校准",用于框架自检)

| 检查项 | 标准 |
|---|---|
| C0 | 全部断言通过,strict 下 tag 不变性 100% |
| Δins/op | 与 design.md §8.1 完全一致(x86:5/4,ARM:4/3;±0) |
| C2 表全热吞吐 | Δ ≤ 3 c/op(两架构) |
| C2 表全热依赖链 | Δ ≤ 10 c/op(x86)/ ≤ 8 c/op(ARM) |
| C4 K≤4096(表 ≤32KB) | Δ ≤ 3 c/op |
| C4 K=1M / C5 全冷 | Δ ≈ +1 次表线访存延迟(120~250c 量级) |
| C8 块级转换 | 热表 Δ% ≤ 5% |
| C14 计算内核 | Δc/op 显著低于 C1(目标 ≤ 1 c/op) |
| 统计稳定性 | 90% 配置 MAD/median ≤ 5% |

---

## 7. 风险与注意事项

- **频率漂移**:锁频 + ABBA 成对做差;记录实测频率列;
- **AArch64 计时陷阱**:cntvct ≠ CPU cycles,一律以 PMU CPU_CYCLES 为准(§3.1);
- **`dc civac` 权限**:依赖 SCTLR.UCI(Linux 默认开放),失败时回退 evict_llc;
- **PMU 多路复用**:事件数超过硬件计数器时内核分时复用,引入误差;按 4~6 个一组分批测量并重复,或只用固定计数器 + 2 个可编程计数器;
- **寄存器驻留可用性**:`-ffixed-r15` 仅 GCC 支持(x86-64 clang 不支持),AArch64 上 GCC/clang 均支持 `-ffixed-x27`;CI 按编译器选择 `pinned` / `mat` 构建并分别记录,不与"转换质量"结论混同;
- **编译器**:锁定 GCC/Clang 版本并存档 codegen;`asm volatile` 阻止跨访存 CSE,纯 C 版对照需注意此差异(design.md §6);
- **预取器**:随机 pattern 下硬件预取基本无效;若需绝对纯净可用 MSR 关预取(可选,默认不关);
- **TLB/页效应**:C4 必须区分表足迹与数据足迹(C12),否则阶梯曲线会混入 dTLB 分量;
- **计数器权限**:perf_event_paranoid ≤ 1 或 root;
- **微架构覆盖**:同一架构至少测 2 代核(如 x86: Skylake 一代 + Zen/最新一代;ARM: A76 类 + V 系),C13 结论才具迁移性。

---

## 8. 实施顺序(里程碑)

| 里程碑 | 内容 | 出口标准 |
|---|---|---|
| M1 | harness(计时/PMU/状态控制)+ C0 | C0 全过;Δins 与 design.md §8.1 一致 |
| M2 | C1/C2/C3/C4(默认变体) | 阶梯曲线三平台特征可辨;C2 达标 |
| M3 | C5–C12、C14 全用例 | 验收表全 PASS 或记录偏差原因 |
| M4 | C13 变体矩阵 + analyze.py 报告 | 产出最终设计裁决(编码/tag/基址/扩展选项) |
