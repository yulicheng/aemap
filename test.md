# test.md — 测试框架与测试用例

> 本文档详细描述 VVA→VA 动态地址映射的 overhead 测试框架与全部测试用例。
> 被测对象的设计（表项编码、快/慢路径、运行时库）见 [design.md](design.md)。
> 测试框架为**统一的一套**（readme 要求）：同一框架、同一用例、同一参数矩阵与指标
> 口径覆盖 x86-64 与 aarch64（ARMv9 + SVE2）两个实现版本，结果直接对比（§3.1）。

## 1. 测试目标与指标

**目标**：量化"软件查表转换"相对直接访存带来的 overhead，并分解为：

- **快路径成本**：表项 L1 命中时每访存新增的指令与依赖链；
- **表 cache 成本**：表项 miss L1/L2/LLC/DRAM 时新增的访存延迟；
- **慢路径成本**：映射创建（含 10us/100us/1ms/10ms 模拟时延）与并发竞争；
- **多线程效应**：共享表的扩展性、同索引创建串行化、churn 稳态开销。

**指标**（每用例每配置）：

| 指标 | 含义 |
|------|------|
| `ns_per_op` / `cycles_per_op` | 每访存耗时（主指标） |
| `overhead_pct` | 相对基线 B0 的开销百分比：`(T_conv − T_base) / T_base × 100%` |
| `instr_per_op` | 每访存指令数（B1 应为 B0 + `VAMAP_CONV_INSNS`；x86-64 strict 默认 8、fast 7，表基址驻留寄存器；使用 BMI2 `shrx` 的变体为 strict 7、fast 6；aarch64 标量为 5；aarch64 SVE2 批量变体为每批口径 fast 6 / strict 7，摊销断言见 §9.3；B2 另含慢路径指令） |
| `l1m/llc/dtlb/br_per_op` | 每访存 L1 miss、LLC miss、dTLB miss、分支 miss |
| `creates / destroys` | 慢路径事件计数（与解析预期值比对，一致性校验） |
| `slow_ns_total / slow_pct` | 慢路径累计耗时（多线程为各线程求和）；`slow_pct = slow_ns_total / (wall_time × threads)`（单线程即占墙钟比例），另报 per-thread 分布（§8） |

## 2. 被测路径与基线

| 编号 | 被测路径 | 说明 |
|------|----------|------|
| B0 | 直接 VA 访存内核 | 同一访存序列、直接指针，无转换 —— overhead 的分母 |
| B1 | 转换 + 0% 无效 | 触及表项全部有效（必要时 alias 预填），纯快路径成本 |
| B2 | 转换 + 按矩阵无效比例与时延 | 完整快慢路径混合 |

公平性：B0 与 B1/B2 使用同一 VVA 序列、同一编译单元与优化等级；对随机/不规则
pattern，三者都从预生成的地址数组取每步 VVA/VA（列表 load 计入基线），规则 pattern
使用相同的指针算术；唯一差异是 B1/B2 在访存前多一次转换。
B0 的 VA 列表由同一转换函数预计算（不计时）；B0/B1/B2 使用同一数据布局（同一 arena
时即同一物理行）。B0 的预计算不得污染 B1/B2 的初始无效状态：B0 使用独立 runtime
实例，或在预计算后按 §4.2 销毁 B0 创建的表项并重建初始状态；若运行时实例不同，记录
布局差异，不声称逐行一致。B0 的 VA 与 B1/B2 的转换结果按同一软件模型核对。慢路径
主导的配置（如 100% 无效）以 creates 与总时间为主判据，B0 仅作稳态段对照。

校验点：**B1 在四种时延下结果必须一致**（0% 无效时慢路径不触发），作为框架自检。

## 3. 框架总体设计

```
vamap_bench
 ├── harness      矩阵生成 / 调度 / 重复运行 / 基线交错 / 结果聚合
 ├── patterns     T1–T15 访存内核（每内核提供 B0/B1/B2 三种指针模式）
 ├── state        表状态控制：按比例设置初始无效状态（rand/target）、alias 预填、每次重复前重置
 ├── timer        架构抽象计时（x86-64：rdtsc+lfence；aarch64：cntvct_el0）+ wall clock；
 │                perf_event_open 或 perf stat 封装
 ├── runtime      vamap 运行时控制：latency/mode、stats 读取、destroy 注入
 └── report       CSV/JSON 输出、汇总表、绘图脚本（matplotlib）
```

CLI（`bench/` 产物）：

```text
vamap_bench --suite correctness                     # 正确性门槛（两架构各自先过）
vamap_bench --test T6 [参数...] [--out results/t6.csv]
vamap_bench --matrix --level smoke|standard|full --out results/
vamap_bench --report --src DIR [--src DIR...] [--compare-arch]   # 汇总/绘图（§11）；
                                                                 # --compare-arch 跨架构对比
参数：--threads N[,N…]  --idx-mode private|shared  --invalid 0|0.25|0.5|0.75|1
      --invalid-scope rand|target  --latency 10us|100us|1ms|10ms
      --latency-mode spin|sleep  --alias 0|1
      --ops N  --reps N  --max-creates-per-rep C  --pin 0-7
```

### 3.1 双架构统一测试（readme 要求）

同一套框架源码覆盖 x86-64 与 aarch64 两个实现版本，任何用例、参数、指标与输出
schema 均不因架构而异：

- **同一构建入口**：x86-64 主机上 `make`、aarch64 主机上原生 `make`（或交叉编译后拷贝
  到目标机执行）产出同一 `vamap_bench` CLI；两实现共享 `include/vamap.h` 的公共 API
  与架构无关运行时 `src/vamap_runtime.c`（design.md §3），仅快路径内联分支按架构选择
  （design.md §5.2/§5.3）；
- **执行顺序**：每个架构先在目标机**原生**跑 §5 正确性门槛（C1–C7 全过），再跑同一
  §4.1 矩阵；矩阵取值、执行流程、重复与噪声控制策略两架构完全相同；
- **构建变体**：`strict` / `fast-tag0` 为两架构公共变体；`bmi2` 仅 x86-64，
  `sve2-batch` 仅 aarch64（design.md §5.2/§5.3）。**跨架构主对比口径**为两架构同源的
  strict 标量/C 形态；架构最优变体（readme：x86-64 BMI2、aarch64 SVE2）单独列示，
  展示最优指令收益，不与另一架构的变体混比；
- **平台差异只进元数据**：REGION_COUNT 回退档位（design.md §6.2）、SVE2 实测 VL
  （`rdvl`）、大页驻留方式等记录在结果元数据（§11），不改变矩阵结构与指标口径；
- **性能必须在原生主机测量**：禁止用模拟器（qemu 等）测性能——时钟校准与 perf 计数
  不可信；模拟器仅可运行正确性套件（C1–C7）并在 `notes` 标注；
- **结果合并**：两台主机的 results 目录按 CSV 的 `arch`/`build` 列合并
  （`--report --compare-arch`，§11 图 5），保证 readme 要求的"测试结果可直接对比"。

## 4. 参数矩阵与执行语义

### 4.1 矩阵（readme 要求）

| 因素 | 取值 |
|------|------|
| 初始无效比例 | **0% / 25% / 50% / 75% / 100%** |
| 映射建立时延 | **10us / 100us / 1ms / 10ms** |
| 线程数 | 1 / 2 / 4 / 8 / 16 |
| 访问模式 | T1–T15（§6） |

- 每个（pattern × threads）跑 **5 × 4 = 20** 个（ratio × latency）组合；
  0% 无效也必须在四种时延下各跑一次（作为 §2 的 B1 正交性校验）；报告中可注明
  其理论结果相同，但不得用复制数据替代实测；
- 三档规模：`smoke`（每 pattern × ratio × latency，threads=1,4）、
  `standard`（+ threads 1/2/4/8）、`full`（全矩阵 + 16 线程 + 变体）。
- T11（首触竞争）与 T12（churn）的无效压力由用例自身注入（T11 的竞争语义以目标表项
  失效为前提，T12 持续销毁–重建）；矩阵的 `invalid_ratio` 对它们只影响初始状态与
  0%/100% 对照，不改变用例的稳态语义。
- 矩阵与架构无关：x86-64 与 aarch64 两个实现版本跑**同一矩阵**（readme：统一一套
  测试框架），每个架构先过 §5 正确性门槛；平台相关差异只记录在结果元数据
  （§3.1/§11），不改变矩阵取值。

### 4.2 无效比例语义

- **初始状态**：在计时外只预填**初始有效**的那部分触及表项（为其分配 region），
  其余保持未映射（`entry=0`）；不要"先全建再置无效"，否则被置 0 的表项仍占着
  region，会在首触时额外消耗 region 而提前耗尽。`invalid_ratio` 是"初始无效表项占
  触及集合的比例"。`--alias 0` 时每个预填 idx 使用唯一 region（数据不混叠；可临时把
  `create_latency_ns` 置 0 走 `vamap_map_create`），`--alias 1` 时共用下文的小 region 池
  （数据可能混叠，仅用于表 cache/overhead 测量）。预填在计时外完成，预热前清零 stats
  （故不计入 `creates`），也不付出预填条数 × create_latency 的时延：
  - `rand`：从该 pattern 的候选索引空间（`--alias 0` 时为 M 个可触达表项；`--alias 1`
    全表形态为全部 `2^20` 个表项）中随机选 `(1 − ratio)` 比例预填为有效，其余保持未映射
    （初始有效性分布与"先全建再随机置无效"相同，但不产生孤儿 region）。实际触及子集内的无效比例由
    `invalid_ratio_actual` 记录；对随机访问类 pattern，预期
    creates = invalid_ratio_actual × 实际触及的不同表项数；
  - `target`：只在 pattern 将触及的表项集合中按确定规则选 `round(ratio × |T|)` 个成员
    保持未映射（首触类场景可控、比例精确）；
- **比例可实现性**：`target` 模式及"4 份等价表项"变体下，触及集合大小为 `|T|` 时实际
  失效数为 `round(ratio × |T|)`，结果记录实际值；`rand` 模式记录
  `invalid_ratio_actual`。该比例定义为**初始状态为无效且本轮被触及的不同表项数** / 实际触及的
  不同表项数（在重置后、计时前统计，避免与运行期 create 循环定义）；
  `|T| < 4` 的 pattern（如 T1、T10 的 W=1、T11-shared、T13/T14 的单表项形态）使用"4 份等价表项"变体
  （同一内核轮转 4 个等价映射）来精确覆盖 0/25/50/75/100%，原始单表项微变体只报告 0/100%；
- **全表有效/大表扫描**：当触及表项数超过可用 region 数（上限为 `REGION_COUNT − 1`，
  平台相关，见 design.md §6.2 回退表：LA57/ARM64 52 位 VA 至多 `2^17−1`、ARM64 48 位 VA
  至多 `2^15−1`、x86-64 4 级页表至多 `2^14−1`；严格 48 位模式再取 `2^16−1` 与
  `floor((2^48 − arena)/4 GiB) − 1` 的最小值）时：
  - `--alias 1` 预填把多个 idx 指向一个小 region 池（默认 64 个 region，轮转分配），
    使 `2^20` 表项在 0% 无效的全表形态下全部有效且数据工作集固定；
  - 对 `invalid_ratio > 0`，慢路径首次创建同样从该池**按引用计数复用** region
    （"create 仍计数、仍付模拟时延，但不消耗新 region"），否则 100% 无效的 `2^20`
    表项需要 `2^20` 个 region，必然耗尽 region 号。该池复用模式中任意数据别名只用于
    表 cache/overhead 测量，不参与正确性判定；共享池 alias API 与引用计数由 C1/C7 在受控池上
    逐项建模校验，C2 用非 alias 路径验证创建/销毁；
  - `--alias 0` 时，框架把触及表项集合的基数限制在 `REGION_COUNT − 1` 以内
    （初始有效的预填项与首触新建项合计各占一个 region），并在结果中记录实际集合大小；
- **每次重复前重置表状态**：把本轮首触新建的表项回滚为未映射（走 `vamap_map_destroy`
  释放/归还 region，不得只把 `entry` 置 0 造成泄漏）、按需重布失效集合 —— 保证瞬态行为
  可复现，每次重复测得相同的"瞬态 + 稳态"混合。`--alias 1` 的表项共享池内 region
  （默认 64 个），重置时按引用计数逐个 `vamap_map_destroy`（最后一个引用才真正回收）；
  若池 region 在多次重复间保留，则只清表项并把池引用计数同步重置到基线值；**不得**
  绕过引用计数直接释放共享 region（design.md §6.4）；
- **持续无效压力**由 T12（churn）承担，其余用例测"初始无效 → 首触创建 → 稳态"全过程。

> 本文的 `2^20`、`0xFFFFF` 等常量均按默认 `VAMAP_IDX_BITS = 20`；若构建改动索引位宽，
> 测试框架的索引范围、掩码与 alias 预填数量须同步更新。

### 4.3 时延模式语义

| 模式 | 行为 | 适用 |
|------|------|------|
| `spin`（默认） | rdtsc 校准忙等，占核，CPU 时间精确计入 | 转换 overhead 主测量 |
| `sleep` | clock_nanosleep 让核 | 对照：系统调用型分配代价（墙钟含调度延迟） |

`sleep` 模式下条带锁同时改为可睡眠互斥锁（不得持自旋锁睡眠），同条带等待者睡眠而非
空转；T11 的墙钟还会计入唤醒/调度延迟（通常高于 spin），但 CPU 占用显著降低。

### 4.4 运行时间预算（自适应缩规模）

最坏组合（随机全空间 × 100% 无效 × 10ms）若触及全部 2^20 表项需 `2^20 × 10ms ≈ 2.9 h`。
框架按 `--max-creates-per-rep`（默认 4096，即 ≤ 41 s/rep）自动缩小该配置下的
**可触及表项空间（索引掩码范围）大小** M：使预计创建数
`invalid_ratio × D(M)` 不超过预算，其中 D(M) 是该 pattern 在本配置下会触及的不同
表项数（均匀随机全空间约为 M；T2 取索引掩码范围；T3–T6 取分块数；T7 取
`M/gcd(S,M)`；T8/T9 按实际访问序列统计；T10 取工作集 W；target 模式按预置集合
精确计算）。
取最大的 2 的幂（至少 1；`invalid_ratio = 0` 不缩小，保持全空间），并在结果中记录
实际 M；预期 creates 同步按解析式修正（§9.3）。缩小只影响可触达的索引空间：
pattern 的索引掩码/活动集合同步改为 M（如 T2 的 `& (M−1)`、T3–T6 的分块数、
T10 的工作集档位），并在结果中记录；实际触及的不同表项数 D ≤ M 由访问序列决定，
不改变慢路径语义。表 cache miss
特性主要由 **0% 配置**（T2/T9/T10 各自的完整工作集，不缩规模）保证；任何时延下只要
预计 create 数超过预算就会缩规模（默认 4096 下全空间随机类 pattern 的
25%/50%/75%/100% 都会缩到 `M ≤ 2^14`，与 10us/10ms 无关），此时表工作集已非全空间，
主指标应解读为慢路径成本/事件率而非全表 cache 行为；
需要全空间非零比例时必须显式调大 `--max-creates-per-rep` 并相应接受更长运行时间。
该上限同样适用于 T11 的 disjoint 首触集合、T8/T9/T10 的大规模首触以及
T12 的连续 churn：通过缩小集合或测量窗口控制每重复 create 次数，不改变 per-create
成本与稳态比率。`smoke` 档
进一步把 `--max-creates-per-rep` 降到 64、reps 降到 2（慢组合每重复 < 1 s），使 CI
总时长保持在约 20 分钟。

## 5. 正确性套件（性能测试门槛）

| ID | 用例 | 通过判据 |
|----|------|----------|
| C1 | 转换位精确性：先设 `cfg.alias_pool_size = 64`，用 `vamap_map_alias` 把全部 `2^20` 个表项按确定规则（如 `region = idx % 64 + 1`）预填到已知 region 池（模型同步记录 idx→region），再在非零 arena 基址下对 10^7 个随机 VVA（含随机 tag[63:52]；offset 覆盖全 32 位，含 bit31=1）逐一转换，与软件模型逐位比对 | 完全一致；tag 不影响结果；VA = arena + region<<32 + offset；转换不触发慢路径、不耗尽 region |
| C2 | create/destroy 往返：无效→创建→有效→销毁；销毁前该 region 号不被其他 idx 复用 | 状态机正确 |
| C3 | 同索引竞争创建：P 线程对同一 VVA 并发首触 | 仅 1 次 create、1 个区域号、各线程 VA 一致、数据一致 |
| C4 | 异索引并发 + 跨线程可见性：A 线程创建映射并写 payload，release 发布 ready；B 线程 acquire 读 ready 后经 `vva2va` 读同一映射并校验 payload | 读到发布值；TSAN/Helgrind 无告警 |
| C5 | churn 压力：8 线程 10^6 次随机 create/destroy 往返（活跃表项数受控 ≤ `REGION_COUNT−1`）+ 并发读者校验 payload 签名（遵守 design.md §6.4 的销毁纪律） | 无错误、无死锁 |
| C6 | 时延精度（SPIN 模式）：4 档时延下实测 spin_ns/creates 与配置偏差 | ≤ ±5% |
| C7 | alias 共享与销毁：两个 idx alias 同一 region，销毁其中一个后另一个仍有效；最后一个销毁后 region 才可复用 | 引用计数正确、无提前回收 |

> 正确性套件默认 `create_latency_ns = 0`（C6 显式遍历 4 档时延）；否则 C5 的
> 10^6 次 create/destroy 会被模拟时延放大到小时级。C1 的 alias 预填也不计时延。
> C1 除 10^7 随机向量外，还显式覆盖边界组合：idx ∈ {0, 2^20−1}、offset ∈ {0, 2^32−1}、
> tag ∈ {0, 全 1}。随机/全 1 tag 的断言只适用于 strict 构建；`VAMAP_FAST_TAG0`
> 构建须把 tag 固定为 0 再跑，否则按设计预期失败。

## 6. 性能测试用例

默认元素 8 字节；`ops` 默认 2^26/次/重复，指**每线程**的 ld/st 次数（多线程总访存
= threads × ops）；多线程下主指标按总访存数归一化，另报 per-thread 分布（§8）。
除 T11/T12 外均为稳定内核（循环体固定）。每个用例同时在 B0/B1/B2
模式下编译运行，`overhead_pct` 相对 B0。B1/B2 默认使用 design.md §5.6 的 volatile
融合宏，保证每次 ld/st 都执行一次转换（禁止 CSE 与循环提升）；aarch64 的
`sve2-batch` 变体使用同构的向量融合宏（design.md §5.3），B0 采用同构向量访存内核，
保证指令断言的差值只含转换；hoist 对照只在 T15 中单独测量。

### T1 热点相邻访问（表项 L1 命中）

- **目的**：测纯快路径成本（readme：重复访问相近 VVA 使映射表 L1 命中）。
- **序列**：在单个映射内的 W 字节窗口反复读写，W ∈ {64B, 4KiB, 2MiB}；
  `for i: A[i % (W/8)] += c;`
- **表行为**：1 个表项常驻 L1；64B/4KiB 窗口的数据行常驻 L1，2MiB 窗口的数据行
  常驻 L2/LLC（视主机缓存容量，可能下探 DRAM），但表项访问始终是 L1 命中。
- **预期**：0% 无效时转换增量 +5–15 cycle/op（§10）；矩阵中的非零比例会叠加首触
  创建成本，需与 0% 配置分开解读。2MiB 档的绝对耗时受数据 miss 影响，
  但该部分在 B0 中同样存在，overhead 仍主要来自转换。此处 `cycle/op` 按每次
  ld/st 计；T1 的 `A[i] += c` 每迭代含 1R+1W 两次访存。基准的融合 VLOAD/VSTORE 宏
  （design.md §5.6）为 volatile，禁止编译器对同一地址的转换做 CSE，故每迭代转换开销约为 2×cycle/op；
  若改用可 CSE 的 C 版转换，编译器可能复用转换结果，此时以 `instr_per_op` 实测为准。

### T2 随机全空间访问（表项 cache miss）

- **目的**：表项 miss 各级缓存的行为 + 慢路径混合（readme：随机访问 VVA 使映射表 miss）。
- **序列**：`idx = xorshift64() & (M−1); off = (rand32() & (DATA_WINDOW−1)) & ~7;`（默认 `M = 2^20`）
  读写随机混合（R:W = 2:1）；`DATA_WINDOW` 默认 4 KiB（可配置），为字节窗口；
  `off` 是 8 字节对齐的字节偏移（元素 8 字节，不跨出窗口边界）。注意只有配合
  `--alias 1` / alias 池时，数据工作集才是 `池 region 数 × DATA_WINDOW`（默认 ≈256 KiB，
  小而热），从而把增量主要归因于表项 miss；非 alias 路径的数据足迹随触及 region 数增长，
  需按实测单独报告。另设全散列变体作端到端对照（数据 miss 与表项 miss 叠加，B0/B2 同样
  存在）：同样走 alias 池（64 region），把 `DATA_WINDOW` 放大到超过 LLC（如 64 MiB，
  按主机内存调整并记录，总足迹 ≈ 64 × DATA_WINDOW），offset 在该窗口内全随机；不要
  直接用 `rand32()` 展满 64×4 GiB 的池地址空间，否则随机访存会触碰数十 GB 物理页，
  缺页/内存带宽会主导结果。
- **表行为**：8 MiB 表超出典型 L2（0.5–2 MiB），通常 L2 必 miss；LLC/DRAM 视主机
  LLC 容量而定；无效比例触发创建。
  全空间 0% 无效/全表扫描用 `--alias 1` 预填；若显式调大 `--max-creates-per-rep`
  保持全空间，`invalid_ratio > 0` 时慢路径创建也复用该 alias 池（§4.2），避免 `2^20`
  表项耗尽 region 号并固定数据工作集。alias 主配置的
  数据窗口小而热，表项 miss 是主要增量；全散列变体的数据 miss 与表项 miss 叠加，但同样
  出现在 B0 中，overhead 只计转换增量；数据页在预热阶段按实际访问集合预触，缺页开销
  单列（§9.2）。
- **预期**：0% 时 +30–60 cycle/op（表项 LLC 命中；LLC 不足 8 MiB 时 +100–200，DRAM）；
  100% × 10ms 时总时间 ≈ creates × 10ms + 稳态时间。

### T3–T6 STREAM 四种访问模式

经典 STREAM 内核的 VVA 版本。默认 **STREAM-M 变体**——把每个数组切成 M 个 16 MiB 分块
（默认 M = 256，即每数组 M × 16 MiB = 4 GiB）。每个数组的第 j 块使用映射索引 j；
多数组内核中各数组的第 j 块共用索引 j 的 4 GiB 窗口，以不同 offset 排布，故触及表项数
恒为 M。按轮转顺序流式扫过，使触及表项数可控、无效率矩阵有意义；`M=1` 时只保留一个
热表项，作为经典 STREAM 的微变体 T3a–T6a（此时仅 0%/100% 两种无效率有意义，其余比例
退化，需按 §4.2 用 4 表项变体覆盖 25% 步进）。

块内仍连续流式：第 j 块第 k 个元素的 VVA 为 `(j << 32) + off_array + k*8`（A/B/C 的
`off_array` 取 0/16 MiB/32 MiB），不是单一线性的 `A + i`；v2 巨数组才用线性 `A + i`
跨越索引边界。

> 注：本节的 M 指 STREAM 分块数。对 T3–T6，它同时也是可触及表项数，故 §4.4 的
> 缩规模直接作用于该分块数（默认 256 远低于默认 create 预算，通常不触发）；§9.3
> 的 M 按此代入，勿与 T2/T10 的索引空间混淆。

| ID | 内核 | 每迭代访存 | 转换次数 |
|----|------|------------|----------|
| T3 | Copy：`A[i] = B[i]` | 1R + 1W | 2 |
| T4 | Scale：`A[i] = q·C[i]` | 1R + 1W | 2 |
| T5 | Add：`C[i] = A[i] + B[i]` | 2R + 1W | 3 |
| T6 | Triad：`A[i] = B[i] + q·C[i]` | 2R + 1W | 3 |

- **变体**：
  - **v2 巨数组**：单数组 > 4 GiB（如 8 GiB），按线性连续 VVA（`A + i`）排布，跨
    `ceil(size/4 GiB)` 个索引，考察索引边界跨越；这与 STREAM-M 的 16 MiB 分块布局不同，
    结果需注明；
  - **v3 首触**：初始 100% 无效，创建成本按首触摊销（与矩阵的 target 模式联动）。
- **表行为**：M 个热表项（默认 256 条 × 8 B = 2 KB，常驻 L1）；数据侧每数组 M × 16 MiB
  （默认 4 GiB）远超各级缓存、为 DRAM 带宽受限（STREAM 的测量本意），单个 16 MiB 分块
  也超出 L1/L2；M=1 时每数组 16 MiB，T3/T4 总工作集 32 MiB、T5/T6 为 48 MiB，可驻留
  大型 LLC（视主机 LLC 容量而定），小 LLC 上仍会下探 DRAM。若主机内存不足以容纳默认
  工作集，可等比缩小分块大小（如 1 MiB）并记录实际值；保持 M 与访问模式不变，带宽
  结论需注明工作集大小；
- **预期**：0% 无效时带宽下降 5–30%（每访存多 1 次 load 占用 load 端口 + 转换链）；
  非零比例配置另计首触创建成本，不与 0% 带宽结论混读。

### T7 步长访问

- **目的**：隔离"表项步长"对表 cache 的影响（与数据访问解耦）。
- **序列**：T7 使用单 region 池（`cfg.alias_pool_size = 1`，或把所有 idx `vamap_map_alias`
  到 region 1；bench alias 默认的 64 region 轮转池不满足"全部指向同一 region"），数据侧
  固定访问该 region 内的同一 64B 窗口；只让表项索引按步长 S ∈ {1, 8, 64} 前进：
  `idx = (i×S) & (M−1)`（默认 `M = 2^20`，重组合按 §4.4 缩小），`off` 固定。另设一组
  不 alias 的对照（数据也随 idx 跨 4 GiB 区），仅用于量化数据侧 miss 的叠加，
  不作为表项步长结论；该对照需把实际触达的不同 idx 数（S=1 时为 M，S>1 时约
  `M/gcd(S,M)`）限制在 `REGION_COUNT − 1` 以内（记录实际 M），否则无法为每个触及 idx
  分配唯一 region。
- **预期**：S=1 时 8 个表项共享一行，表项行顺序流，预取器最有效；S=8 时每次访问新行
  但行地址连续，预取可能仍部分有效；S=64 时行间距 512B，预取收益最低、开销最高。
  alias 组数据地址恒定，差异只来自表项行/预取行为；表项步长结论取 0% 无效配置，
  矩阵中的非零比例会叠加首触创建成本，需分离后再比较。

### T8 指针追逐

- **目的**：转换位于关键路径时的最大延迟暴露（无 ILP 摊销）。
- **序列**：链表节点散布于大量映射（随机 idx；超过 region 号容量时用 `--alias 1` 预填，
  但每个节点必须落在池内唯一的 `(region, offset)` 组合，避免不同 VVA 别名到同一物理
  节点而破坏链——最简单是让每个节点的 offset 全局唯一，则无论落到哪个 region 都不冲突；
  节点内存放的是下一节点的 **VVA**，不是 VA），
  `p = *(void**)vva2va(p)` 追 R 轮 × N hop；对照 B0 为同构链表直接追逐（存 VA）。
- **预期**：每 hop 延迟完整叠加表项访问延迟。100% 无效的创建成本只发生在**首轮**：
  报告须把首轮与后续轮分开，或在每轮前重置表项（churn），否则 R 轮的摊销会把
  "0% vs 100% 差异最大"稀释成 1/R。

### T9 热/冷分布（Zipf）

- **目的**：真实负载的混合命中分布。
- **序列**：Zipf(s) 键分布（s ∈ {0.9, 1.1}，覆盖 2^16 个表项；`--alias 1` 预填以避免
  region 号耗尽；非零比例配置按 §4.4 可能缩小 M 并在结果记录，是否缩小取决于
  `ratio × M` 与预算而非 latency），按键访问；
  每个键使用固定的小 offset 窗口（数据工作集 = 池 region 数 × 窗口，默认 256KiB，
  保持热，避免数据 miss 混入表项分布结论），另设数据也随键变化的端到端对照（仍走
  alias 池，只让 offset 随键变化，避免耗尽 region）；热/冷切分 90/10 与 99/1 两种报告口径。
- **预期**：开销介于 T1 与 T2 之间，且对热集是否驻留 L1 敏感。

### T10 表工作集扫描

- **目的**：刻画表项驻留台阶（L1 → L2 → LLC → DRAM）。
- **序列**：活跃表项数 W ∈ {1, 8, 64, 512, 4K, 32K, 256K, 1M}（重组合按 §4.4 截断到
  M），表项索引均匀随机（主配置 0% 无效；矩阵中的非零比例按 §4.2 预置初始无效并在
  首触创建，曲线解读时须分离慢路径成本；W 超过可用 region 数时用 `--alias 1` 预填）；
  数据侧固定访问同一小窗口（alias 组所有 idx 指向同一 region 池、offset 固定；实际
  数据工作集 = 池 region 数 × 窗口，默认 64×4KiB=256KiB，保持热），只改变表项工作集 W。
  另设一组数据也随索引变化的端到端对照（同样走 alias 池，只让 offset 随索引变化），
  但不作为表项驻留台阶结论；每档测 cycles/op 曲线。
- **预期**：W ≤ 4K（8B×4K=32KB，约 L1 容量）时 L1 台阶；32K（256KB）进入 L2，
  256K（2MB）到 L2/LLC 边界，1M（8MB）进入 LLC/DRAM，逐级抬升。

### T11 首触风暴（多线程创建竞争）

- **目的**：慢路径竞争与串行化语义（readme：多线程共享相同表项）。
- **序列**：barrier 后所有线程同时首触：
  - **shared**：同一无效索引（表项锁串行化）——SPIN 模式下预期墙钟 ≈ latency × 1 +
    (P−1)×锁交接（SLEEP 见 §4.3），creates = 1（目标初始无效时；0% 对照为 0，见 §4.1）。
    中间比例按 §4.2 用 4 表项变体时，各线程并发触及同一 4 表项集合：每个初始无效
    表项恰被创建 1 次（同索引在条带锁上串行化，落入不同条带的表项可并行创建），
    故 creates = round(ratio × 4)，与 §9.3"每个目标索引 1 次"的口径一致；
  - **disjoint**：各线程私有无效索引集（跨条带并行）——预期近线性扩展，
    creates = P × 各线程私有集合中的初始无效数（0% 对照为 0）。
- **线程**：P ∈ {2, 4, 8, 16}；时延取全矩阵。disjoint 模式下所有线程的集合总规模按
  `--max-creates-per-rep` 限制（总 creates ≤ 上限，§4.4），避免 10ms 档放大总时长。

### T12 销毁–重建 churn（持续无效压力）

- **目的**：稳态下的持续慢路径流量与销毁路径成本。
- **序列**：`每 K 次访存销毁刚访问的表项 → 下次访问重建`，
  K ∈ {16, 256, 4096, 65536}；单线程 + 多线程；遵守 design.md §6.4 的销毁纪律
  （销毁仅作用于本线程刚访问且不再被他线程共享的表项，或多线程模式用 target 重布）。
  活跃表项数不得超过可用 region 数，否则用 alias 池或缩小工作集（§4.2）。
  每重复的重建次数受 `--max-creates-per-rep` 限制（§4.4），只缩短测量窗口、不改变
  稳态 1/K 比率；指标按每个 churn 周期归一化。
- **预期**：稳态无效比率 = 1/K，开销随 K 递减趋于 B1。

### T13 读改写（RMW）

- **目的**：单次转换覆盖 RMW 的正确形态与原子操作叠加成本。
- **序列**：`__atomic_fetch_add((uint64_t*)vva2va(p), 1)`（每 RMW 恰 1 次转换），
  对照 B0 为直接地址 fetch_add；另测"每访存各转换一次"的未优化写法作对照。
- **预期**：优化形态 ≈ T1 + 原子操作增量；未优化形态约 2×转换次数。

### T14 gather / scatter

- **目的**：表热 + 数据冷（数据 cache miss 主导）时转换是否被数据延迟掩盖。
- **序列**：单映射内随机下标 `sum += A[idx_list[i]]`（gather）与 `A[idx_list[i]] = x`（scatter）。
- **预期**：表项 L1 命中、数据行 miss → 转换开销大部分被掩盖，overhead 占比低。

### T15 hoist 对比（优化空间上界）

- **目的**：量化"逐次内联转换"相对"循环外提升查表"（`vamap_pin`）的差距，
  给出软件优化方向的收益上界。
- **序列**：同一 STREAM-Triad 内核两种写法对比；只取 Triad 一个内核，但 ratio × latency
  全测（threads 按 §4.1 档位）。
- **预期**：hoist 版 ≈ B0 + 边界检查成本；逐次版 − hoist 版即转换开销中可被优化消除的部分。

## 7. 单线程执行方案

1. 按 §4.1 矩阵遍历（threads = 1）：pattern × ratio × latency；
2. 每配置 `--reps` 次重复（默认 5，T8/T10 建议 10），每次重复前重置表状态（§4.2）；
3. 每次重复内部顺序：重置状态并清零 stats → 预热（仅触碰将访问的有效表项对应数据页，
   不含计时）→ 屏障 → rdtsc 计时内核 → 读 stats；
4. 基线 B0 与被测 B1/B2 **交错运行**（同一重复序号内相邻执行）以对齐频率/噪声漂移；
5. 统计：报中位数、最小/最大、CV；CV > 5% 自动追加重复（上限 20）。

## 8. 多线程执行方案

- **线程布局**：线程绑核（`--pin`），共享映射表与运行时；SPIN 模式下 `threads`
  不应超过可用物理核数（持自旋锁的线程被抢占会污染 T11/T12 墙钟），否则改用
  `sleep` 模式并在结果标注；
- **索引共享模式**（每 pattern 均测两种）：
  - `private`：线程私有 VVA 区间（表项集合不重叠；映射表本身共享且快路径只读，
    预期近线性扩展）；
  - `shared`：线程访问相同表项集合（复现 readme 所述"多线程共享相同表项"，
    快路径无争用、慢路径经条带锁串行化）；
- **重点组合**：T11（竞争语义）、T12（churn）、T2/T6（扩展性）× threads {1,2,4,8,16}；
- **结果**：除单线程指标外，报告加速比曲线与 per-thread cycles/op 分布（p50/p99）；
  创建类场景同时报告 creates 实测 vs 解析预期。

## 9. 测量方法学

### 9.1 计时与计数器

- 计时：内核前后序列化 + 周期计数，按架构抽象（x86-64：`rdtsc` + `lfence`；
  aarch64：`mrs cntvct_el0`，频率经 `cntfrq_el0` 换算）；两架构统一输出
  cycles_per_op / ns_per_op，口径一致；慢路径主导的配置同时记录墙钟；
- 硬件计数器：`perf stat -x, -e cycles,instructions,L1-dcache-load-misses,
  LLC-load-misses,dTLB-load-misses,branch-misses,context-switches`；
  aarch64 上 PMU 事件名/映射可能不同（armv8/v9 PMU），框架按主机探测可用事件，
  缺失项在 `notes` 记录；无 perf 权限（`perf_event_paranoid` 受限）时自动降级为
  纯周期计数并在结果标注；
- 运行时自带计数（slow_calls/creates/spin_ns/slow_ns_total）用于分解与一致性校验；
  对逐次转换内核，`fast_hits` 由框架按"总 ld/st 数 − slow_calls"导出（T15 hoist 版按
  实际转换点数另记）；运行时不在快路径计数（否则会破坏 8 条快路径，见 design.md §6.1）。

### 9.2 噪声控制

- CPU 绑核 + 建议隔离核/`performance` 调频器/固定频率，报告记录环境元数据；
- 映射表尽量以 2 MiB 大页驻留（`MAP_HUGETLB` 失败时回退 `madvise(MADV_HUGEPAGE)` 并记录实际方式，
  避免表自身 TLB miss 混入）；
- 预热提交数据物理页（避免首触缺页混入快路径测量；创建成本属被测对象，保留）；
  初始无效表项创建后的首次数据访问仍可能触发缺页，框架记录 minor/major faults，
  并在结果中单列，避免与配置的创建时延混淆；
- 指令计数用例使用 `-O2 -fno-tree-vectorize`，避免 B0/B1 自动向量化差异破坏
  “转换新增指令数”对应的判定；
- 基线交错运行；丢弃各配置首轮。

### 9.3 一致性校验（结果可信门槛）

| 校验 | 判据 |
|------|------|
| 慢路径计数 | `creates_actual == creates_expected`；均匀随机访问解析式：触及数 `D ≈ M(1−(1−1/M)^N)`，其中 M 为该用例实际的可触及表项空间（T2 为索引掩码范围、T3–T6 为分块数、T10 为工作集 W、T1 的单表项形态为 1 / 4 表项变体为 4），N 为访问次数，`creates_expected = invalid_ratio_actual × D`（T3–T6 按轮转覆盖全部分块时 D=M；T7 步长按 §4.4 解析式 `M/gcd(S,M)` 估计、以实际触及集合校验；Zipf/指针追逐按实际触及集合统计 D；target 模式按集合精确计算；多线程按 `idx_mode` 分别计算：`private` 取各线程在其私有索引空间上的 D 之和，`shared` 用总访问次数在共享索引空间上计算；T11 按 shared=每个目标索引 1 次、disjoint=各线程私有集合之和单独计算；T12 按测量窗口内的 churn 次数计算；比例取整后以实际值为准） |
| 快路径形态 | T1 **0% 无效（B1）** 的 `instr_per_op == B0 + VAMAP_CONV_INSNS`（x86-64 strict 默认 8、fast 7；BMI2 `shrx` 变体 7/6；aarch64 标量为 5；aarch64 SVE2 批量变体按每批断言 `(instr(B1) − instr(B0)) × VL == VAMAP_CONV_INSNS`，fast 6 / strict 7，VL 由 `rdvl` 探测并记录；按构建变体断言） |
| 时延正交性 | B1 在四种时延下 ns_per_op 偏差 ≤ 1% |
| 重复稳定性 | 中位数 CV ≤ 5%，否则加跑 |

## 10. 预期结果与合理性判据

量级预期（3 GHz x86-64 参考量级，具体随微架构/主机变化，用于捕获明显异常；
aarch64 各档 cycle/op 不同，跨架构口径见下）：

| 场景 | 预期 overhead |
|------|---------------|
| T1（0% 无效） | +5–15 cycle/op（≈2–5 ns） |
| T2（0% 无效） | +30–60 cycle/op（表项 LLC 命中）；LLC 不足 8 MiB 时 +100–200（DRAM） |
| T2（100%，10us） | 平均 ≈ creates×10us/N + 快路径成本 |
| T2（100%，10ms） | 总时间被创建主导：≈ creates × 10ms |
| T3–T6（0% 无效，STREAM-M） | 带宽下降 5–30% |
| T7 表项步长 1 vs 64 | 步长 64 的开销显著更高（表行预取失效） |
| T8 | 每 hop 叠加表项访问延迟；100% 无效的创建差异集中在首轮 |
| T10 | L1/L2/LLC/DRAM 四级台阶清晰 |
| T11 shared vs disjoint | shared 墙钟 ≈ latency + 交接；disjoint 近线性 |
| T15 | hoist 版开销 ≈ B0 + 边界检查，明显低于逐次版 |

跨架构口径（readme：两个版本的结果可直接对比）：

- 跨架构对比以 **overhead_pct**（相对各自 B0）为主指标；绝对 cycle/op 不要求跨架构
  一致（微架构、频率、内存序实现均不同），异常检查按上表量级各自独立执行；
- aarch64 标量形态：快路径 5 条、`ldar`/`bfi` 关键路径与 x86-64 同阶，T1（0%）量级
  同样落在 +5–15 cycle/op 附近；`sve2-batch` 变体在吞吐型内核（T2/T3–T6/T9/T10）上
  转换指令增量低于标量形态（ALU/分支按 VL 摊销），但每访存表项 load 次数不变，
  带宽受限内核的 overhead 下界由表项 load 决定。

异常处理：任一结果偏离预期量级 2× 以上 → 记录并复查（快路径形态、频控、表大页）。

## 11. 输出与报告

CSV schema（每行一个配置一次运行）：

```text
suite,host,date,arch,build,test_id,pattern,variant,mode(B0|B1|B2),threads,idx_mode,alias,
invalid_ratio,invalid_ratio_actual,invalid_scope,latency_ns,latency_mode,region_count,arena_base,M,ops,reps,
creates_expected,creates_actual,destroys,
ns_per_op_med,ns_per_op_min,ns_per_op_max,cycles_per_op,instr_per_op,
l1m_per_op,llc_per_op,dtlb_per_op,br_per_op,minor_faults,major_faults,
slow_ns_total,slow_pct,overhead_pct,CV,notes
```

其中 `M` 为 §4.4 的可触及表项空间大小；`region_count` / `arena_base` 记录
`vamap_init` 实际回退到的 region 数与竞技场基址（十六进制），用于复现与解释
strict/平台差异；T3–T6 的 STREAM 分块数、T7 的步长 S、T10 的工作集 W 记录在
`variant` 或 `notes` 中，避免与全局 `M` 混淆；T10 的非零比例配置计算
`creates_expected` 时以 M=W 代入 §9.3 解析式（0% 配置为 0）；`arch`/`build` 记录
目标架构与构建变体（x86-64：strict / fast-tag0 / bmi2；aarch64：strict / fast-tag0 /
sve2-batch），跨架构合并对比按这两列分组，aarch64 的 `sve2-batch` 构建把 `rdvl`
实测 VL 写入 `notes`。

图表清单：

1. overhead 热力图：ratio（5）× latency（4），分 pattern、分 threads；
2. T10 工作集台阶曲线；T8/T14 延迟分解对比；
3. 多线程加速比曲线（private/shared × threads）；
4. T12 开销–churn 间隔曲线；T15 逐次 vs hoist 对比柱状图；
5. 跨架构对比图（readme：统一框架测试两个版本）：同 pattern 同矩阵下 x86-64 与
   aarch64 并列（overhead_pct / cycles_per_op），主口径 strict，bmi2 / sve2-batch
   变体单独列示。

## 12. 命令清单

```bash
# 0) 正确性门槛
./build/vamap_bench --suite correctness

# 1) 单用例（STREAM Triad，4 线程，25% 无效，100us 时延）
./build/vamap_bench --test T6 --threads 4 --idx-mode shared \
    --invalid 0.25 --invalid-scope rand --latency 100us --latency-mode spin \
    --ops 67108864 --reps 5 --pin 0-7 --out results/t6_t4.csv

# 2) 单线程全矩阵
./build/vamap_bench --matrix --threads 1 --level full --out results/st/

# 3) 多线程标准档
./build/vamap_bench --matrix --threads 1,2,4,8 --level standard --out results/mt/

# 4) 快路径形态抽查
./scripts/check_fastpath.sh build/vamap_bench

# 5) aarch64 主机（原生，同一框架、同一 CLI、同一矩阵；readme 统一测试框架要求）
./build/vamap_bench --suite correctness
./build/vamap_bench --matrix --threads 1,2,4,8 --level standard --out results/aarch64/

# 6) 合并两架构结果做跨架构对比（§11 图 5；按 arch/build 列分组）
./build/vamap_bench --report --src results/x86-64/ --src results/aarch64/ \
    --compare-arch --out results/compare/
```

运行预算参考：`full` 档 ≈ 15 pattern × 20 组合 × 5 线程档 × reps 3–5，
重组合由 §4.4 自适应缩规模控制在每重复 ≤ 1 min 量级；CI 使用 `smoke` 档
（threads 1/4，重组合自动截断）约 20 分钟。
