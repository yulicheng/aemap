# test.md — 测试框架与测试用例

> 本文档详细描述 VVA→VA 动态地址映射的 overhead 测试框架与全部测试用例。
> 被测对象的设计（表项编码、快/慢路径、运行时库）见 [design.md](design.md)。

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
| `instr_per_op` | 每访存指令数（应为 B0 + `VAMAP_CONV_INSNS`；x86-64 strict 默认 8、fast 7，表基址驻留寄存器；使用 BMI2 `shrx` 的变体为 strict 7、fast 6） |
| `l1m/llc/dtlb/br_per_op` | 每访存 L1 miss、LLC miss、dTLB miss、分支 miss |
| `creates / destroys` | 慢路径事件计数（与解析预期值比对，一致性校验） |
| `slow_ns_total / slow_pct` | 慢路径累计耗时及占比（运行时统计） |

## 2. 被测路径与基线

| 编号 | 被测路径 | 说明 |
|------|----------|------|
| B0 | 直接 VA 访存内核 | 同一访存序列、直接指针，无转换 —— overhead 的分母 |
| B1 | 转换 + 0% 无效 | 触及表项全部有效（必要时 alias 预填），纯快路径成本 |
| B2 | 转换 + 按矩阵无效比例与时延 | 完整快慢路径混合 |

校验点：**B1 在四种时延下结果必须一致**（0% 无效时慢路径不触发），作为框架自检。

## 3. 框架总体设计

```
vamap_bench
 ├── harness      矩阵生成 / 调度 / 重复运行 / 基线交错 / 结果聚合
 ├── patterns     T1–T15 访存内核（每内核提供 B0/B1/B2 三种指针模式）
 ├── state        表状态控制：按比例置无效（rand/target）、alias 预填、每次重复前重置
 ├── timer        rdtsc(fenced) 计时 + wall clock；perf_event_open 或 perf stat 封装
 ├── runtime      vamap 运行时控制：latency/mode、stats 读取、destroy 注入
 └── report       CSV/JSON 输出、汇总表、绘图脚本（matplotlib）
```

CLI（`bench/` 产物）：

```text
vamap_bench --suite correctness                     # 正确性门槛
vamap_bench --test T6 [参数...] [--out results/t6.csv]
vamap_bench --matrix --level smoke|standard|full --out results/
参数：--threads N[,N…]  --idx-mode private|shared  --invalid 0|0.25|0.5|0.75|1
      --invalid-scope rand|target  --latency 10us|100us|1ms|10ms
      --latency-mode spin|sleep  --alias 0|1
      --ops N  --reps N  --max-creates-per-rep K  --pin 0-7
```

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

### 4.2 无效比例语义

- **初始状态**：先用 `vamap_map_alias`（不计时延）按 pattern 把其触及的表项预填为有效，
  再按 `invalid_scope` 置无效；`invalid_ratio` 是"初始无效表项占触及集合的比例"。
  预填走 alias 而非 `vamap_map_create`，避免重置阶段就付出
  触及表项数 × create_latency 的总时延：
  - `rand`：从已建立表项中随机选 `round(ratio × 已建立数)` 个置无效（`entry=0`）；若用
    `--alias 1` 全表预填，则等价于从 `2^20` 个表项中随机选。对随机访问类 pattern，
    预期 creates = invalid_ratio_actual × 实际触及的不同表项数；
  - `target`：只在 pattern 将触及的表项集合中选 ratio 比例的成员置无效（首触类场景可控）；
- **比例可实现性**：集合大小为 N 时实际失效数为 `round(ratio × N)`，结果记录实际值；
  N < 4 的 pattern（如 T1、T10 的 W=1、T11-shared、T13/T14 的单表项形态）使用"4 份等价表项"变体
  （同一内核轮转 4 个等价映射）来精确覆盖 0/25/50/75/100%，原始单表项微变体只报告 0/100%；
- **全表有效/大表扫描**：当触及表项数超过可用 region 数（= `REGION_COUNT − 1`，平台相关，
  见 design.md §6.2 回退表：LA57/ARM64 52 位 VA 至多 `2^17−1`、ARM64 48 位 VA 至多
  `2^15−1`、x86-64 4 级页表至多 `2^14−1`；严格 48 位模式另受 `2^16−1` 分配上限约束）时：
  - `--alias 1` 预填把多个 idx 指向一个小 region 池（默认 64 个 region，轮转分配），
    使 `2^20` 表项全部有效且数据工作集固定；
  - 对 `invalid_ratio > 0`，慢路径首次创建同样从该池**按引用计数复用** region
    （"create 仍计数、仍付模拟时延，但不消耗新 region"），否则 100% 无效的 `2^20`
    表项需要 `2^20` 个 region，必然耗尽 region 号。该模式只用于表 cache/overhead 测量
    （数据可能别名到同一区域，不参与正确性判定），映射正确性由 C1/C2/C7 在非 alias 模式覆盖；
  - 不使用 alias 时，框架把触及表项集合的基数限制在 `REGION_COUNT − 1` 以内
    （所有首触完成后这些表项都要各占一个 region），并在结果中记录实际集合大小；
- **每次重复前重置表状态**：创建的表项回滚、失效集合重布 —— 保证瞬态行为可复现，
  每次重复测得相同的"瞬态 + 稳态"混合。alias 表项共享同一 region，重置时按引用计数
  逐个 `vamap_map_destroy`（最后一个引用才真正回收），或只清表项；**不得**绕过引用计数
  直接释放共享 region（design.md §6.4）；
- **持续无效压力**由 T12（churn）承担，其余用例测"初始无效 → 首触创建 → 稳态"全过程。

### 4.3 时延模式语义

| 模式 | 行为 | 适用 |
|------|------|------|
| `spin`（默认） | rdtsc 校准忙等，占核，CPU 时间精确计入 | 转换 overhead 主测量 |
| `sleep` | clock_nanosleep 让核 | 对照：系统调用型分配代价（墙钟含调度延迟） |

### 4.4 运行时间预算（自适应缩规模）

最坏组合（随机全空间 × 100% 无效 × 10ms）若触及全部 2^20 表项需 `2^20 × 10ms ≈ 2.9 h`。
框架按 `--max-creates-per-rep`（默认 4096，即 ≤ 41 s/rep）自动缩小该配置下的
**触及表项集合大小** M 并在结果中记录实际 M；预期 creates 同步按解析式修正（§9.3）。
缩小只影响触及集合：pattern 的索引掩码/活动集合同步改为 M（如 T2 的 `& (M−1)`、
T10 的工作集档位），并在结果中记录；不改变慢路径语义。表 cache miss 特性由
0%/低比例的全空间配置（T2/T9/T10）保证，`100% × 10ms` 等组合的主指标是慢路径成本
而非表 cache 行为。`smoke` 档进一步把 `--max-creates-per-rep` 降到 64、reps 降到 2
（慢组合每重复 < 1 s），使 CI 总时长保持在约 20 分钟。

## 5. 正确性套件（性能测试门槛）

| ID | 用例 | 通过判据 |
|----|------|----------|
| C1 | 转换位精确性：在非零 arena 基址下预填 10^7 个随机 VVA（含随机 tag[63:52]），与软件模型逐位比对 | 完全一致；tag 不影响结果；VA = arena + region<<32 + offset |
| C2 | create/destroy 往返：无效→创建→有效→销毁；区域号不复用至销毁 | 状态机正确 |
| C3 | 同索引竞争创建：N 线程对同一 VVA 并发首触 | 仅 1 次 create、1 个区域号、各线程 VA 一致、数据一致 |
| C4 | 异索引并发 + 跨线程可见性：A 线程创建映射并写 payload，release 发布 ready；B 线程 acquire 读 ready 后读 payload | 读到发布值；TSAN/Helgrind 无告警 |
| C5 | churn 压力：8 线程 10^6 次随机 create/destroy + 并发读者校验 payload 签名（遵守 design.md §6.4 的销毁纪律） | 无错误、无死锁 |
| C6 | 时延精度：4 档时延下实测 spin_ns/creates 与配置偏差 | ≤ ±5% |
| C7 | alias 共享与销毁：两个 idx alias 同一 region，销毁其中一个后另一个仍有效；最后一个销毁后 region 才可复用 | 引用计数正确、无提前回收 |

## 6. 性能测试用例

默认元素 8 字节；`ops` 默认 2^26/次/重复；除 T11/T12 外均为稳定内核（循环体固定）。
每个用例同时在 B0/B1/B2 模式下编译运行，`overhead_pct` 相对 B0。

### T1 热点相邻访问（表项 L1 命中）

- **目的**：测纯快路径成本（readme：重复访问相近 VVA 使映射表 L1 命中）。
- **序列**：在单个映射内的 W 字节窗口反复读写，W ∈ {64B, 4KiB, 2MiB}；
  `for i: A[i % (W/8)] += c;`
- **表行为**：1 个表项常驻 L1；64B/4KiB 窗口的数据行常驻 L1，2MiB 窗口的数据行
  常驻 L2/LLC（视主机缓存容量，可能下探 DRAM），但表项访问始终是 L1 命中。
- **预期**：转换增量 +5–15 cycle/op（§10）；2MiB 档的绝对耗时受数据 miss 影响，
  但该部分在 B0 中同样存在，overhead 仍主要来自转换。

### T2 随机全空间访问（表项 cache miss）

- **目的**：表项 miss 各级缓存的行为 + 慢路径混合（readme：随机访问 VVA 使映射表 miss）。
- **序列**：`idx = xorshift64() & (2^20−1); off = rand32();` 读写随机混合（R:W = 2:1）。
- **表行为**：8 MiB 表超出典型 L2（0.5–2 MiB），通常 L2 必 miss；LLC/DRAM 视主机
  LLC 容量而定；无效比例触发创建。
  全空间 0% 无效/全表扫描用 `--alias 1` 预填；`invalid_ratio > 0` 时慢路径创建复用
  该 alias 池（§4.2），避免 `2^20` 表项耗尽 region 号并固定数据工作集；数据侧随机
  访问的 miss 在 B0/B2 中同样存在，overhead 只计转换增量；
  数据页在预热阶段按实际访问集合预触，缺页开销单列（§9.2）。
- **预期**：0% 时 +30–60 cycle/op（表项 LLC 命中）；100% × 10ms 时总时间 ≈
  creates × 10ms + 稳态时间。

### T3–T6 STREAM 四种访问模式

经典 STREAM 内核的 VVA 版本。默认 **STREAM-M 变体**——把每个数组切成 M 个 16 MiB 分块
（默认 M = 256，即每数组 M × 16 MiB = 4 GiB）。每个数组的第 j 块使用映射索引 j；
多数组内核中各数组的第 j 块共用索引 j 的 4 GiB 窗口，以不同 offset 排布，故触及表项数
恒为 M。按轮转顺序流式扫过，使触及表项数可控、无效率矩阵有意义；`M=1` 时只保留一个
热表项，作为经典 STREAM 的微变体 T3a–T6a（此时仅 0%/100% 两种无效率有意义，其余比例
退化，需按 §4.2 用 4 表项变体覆盖 25% 步进）。

| ID | 内核 | 每迭代访存 | 转换次数 |
|----|------|------------|----------|
| T3 | Copy：`A[i] = B[i]` | 1R + 1W | 2 |
| T4 | Scale：`A[i] = q·C[i]` | 1R + 1W | 2 |
| T5 | Add：`C[i] = A[i] + B[i]` | 2R + 1W | 3 |
| T6 | Triad：`A[i] = B[i] + q·C[i]` | 2R + 1W | 3 |

- **变体**：
  - **v2 巨数组**：单数组 > 4 GiB（如 8 GiB），跨 2+ 个映射，考察索引边界跨越；
  - **v3 首触**：初始 100% 无效，创建成本按首触摊销（与矩阵的 target 模式联动）。
- **表行为**：M 个热表项（默认 256 条 × 8 B = 2 KB，常驻 L1）；数据侧每数组 M × 16 MiB
  （默认 4 GiB）远超各级缓存、为 DRAM 带宽受限（STREAM 的测量本意），单个 16 MiB 分块
  也超出 L1/L2；M=1 时每数组 16 MiB，T3/T4 总工作集 32 MiB、T5/T6 为 48 MiB，可驻留
  大型 LLC（视主机 LLC 容量而定），小 LLC 上仍会下探 DRAM；
- **预期**：0% 无效时带宽下降 5–30%（每访存多 1 次 load 占用 load 端口 + 转换链）。

### T7 步长访问

- **目的**：隔离"表项步长"对表 cache 的影响（与数据访问解耦）。
- **序列**：单数组内步长 stride ∈ {64B, 4KiB, 2MiB, **4GiB**, **32GiB**, **256GiB**}；
  stride ≥ 4GiB 时每次访问落到不同映射索引，表项步长 = stride >> 32 ∈ {1, 8, 64} 条。
- **预期**：表项步长 1 时硬件预取器可顺序预取表行，开销显著低于步长 8/64（每访存新行）。

### T8 指针追逐

- **目的**：转换位于关键路径时的最大延迟暴露（无 ILP 摊销）。
- **序列**：链表节点散布于大量映射（随机 idx；超过 region 号容量时用 `--alias 1` 预填），
  `p = *(void**)vva2va(p)` 追 K 轮 × N hop；对照 B0 为同构链表直接追逐。
- **预期**：每 hop 延迟完整叠加表项访问延迟；0% vs 100% 无效差异最大。

### T9 热/冷分布（Zipf）

- **目的**：真实负载的混合命中分布。
- **序列**：Zipf(s) 键分布（s ∈ {0.9, 1.1}，覆盖 2^16 个表项；`--alias 1` 预填以避免
  region 号耗尽），按键访问；热/冷切分 90/10 与 99/1 两种报告口径。
- **预期**：开销介于 T1 与 T2 之间，且对热集是否驻留 L1 敏感。

### T10 表工作集扫描

- **目的**：刻画表项驻留台阶（L1 → L2 → LLC → DRAM）。
- **序列**：活跃表项数 W ∈ {1, 8, 64, 512, 4K, 32K, 256K, 1M}，均匀随机访问（0% 无效；
  W 超过可用 region 数时用 `--alias 1` 预填），每档测 cycles/op 曲线。
- **预期**：W ≤ 4K（8B×4K=32KB，约 L1 容量）时 L1 台阶；32K（256KB）进入 L2，
  256K（2MB）到 L2/LLC 边界，1M（8MB）进入 LLC/DRAM，逐级抬升。

### T11 首触风暴（多线程创建竞争）

- **目的**：慢路径竞争与串行化语义（readme：多线程共享相同表项）。
- **序列**：barrier 后所有线程同时首触：
  - **shared**：同一无效索引（表项锁串行化）——预期墙钟 ≈ latency × 1 + (K−1)×锁交接，
    creates = 1；
  - **disjoint**：各线程私有无效索引集（跨条带并行）——预期近线性扩展，creates = K×各自集合。
- **线程**：K ∈ {2, 4, 8, 16}；时延取全矩阵。

### T12 销毁–重建 churn（持续无效压力）

- **目的**：稳态下的持续慢路径流量与销毁路径成本。
- **序列**：`每 K 次访存销毁刚访问的表项 → 下次访问重建`，
  K ∈ {16, 256, 4096, 65536}；单线程 + 多线程；遵守 design.md §6.4 的销毁纪律
  （销毁仅作用于本线程刚访问且不再被他线程共享的表项，或多线程模式用 target 重布）。
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
- **序列**：同一 STREAM-Triad 内核两种写法对比，矩阵子集（ratio × latency 全测）。
- **预期**：hoist 版 ≈ B0 + 边界检查成本；逐次版 − hoist 版即转换开销中可被优化消除的部分。

## 7. 单线程执行方案

1. 按 §4.1 矩阵遍历（threads = 1）：pattern × ratio × latency；
2. 每配置 `--reps` 次重复（默认 5，T8/T10 建议 10），每次重复前重置表状态（§4.2）；
3. 每次重复内部顺序：重置状态 → 预热（仅触碰将访问的有效表项对应数据页，不含计时）
   → 屏障 → rdtsc 计时内核 → 读 stats；
4. 基线 B0 与被测 B1/B2 **交错运行**（同一重复序号内相邻执行）以对齐频率/噪声漂移；
5. 统计：报中位数、最小/最大、CV；CV > 5% 自动追加重复（上限 20）。

## 8. 多线程执行方案

- **线程布局**：线程绑核（`--pin`），共享映射表与运行时；
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

- 计时：内核前后 `rdtsc` + `lfence` 序列化；慢路径主导的配置同时记录墙钟；
- 硬件计数器：`perf stat -x, -e cycles,instructions,L1-dcache-load-misses,
  LLC-load-misses,dTLB-load-misses,branch-misses,context-switches`；
  无 perf 权限（`perf_event_paranoid` 受限）时自动降级为纯 rdtsc 并在结果标注；
- 运行时自带计数（fast_hits/slow_calls/creates/spin_ns）用于分解与一致性校验。

### 9.2 噪声控制

- CPU 绑核 + 建议隔离核/`performance` 调频器/固定频率，报告记录环境元数据；
- 映射表以 2 MiB 大页驻留（避免表自身 TLB miss 混入）；
- 预热提交数据物理页（避免首触缺页混入快路径测量；创建成本属被测对象，保留）；
  初始无效表项创建后的首次数据访问仍可能触发缺页，框架记录 minor/major faults，
  并在结果中单列，避免与配置的创建时延混淆；
- 指令计数用例使用 `-O2 -fno-tree-vectorize`，避免 B0/B1 自动向量化差异破坏
  “转换新增指令数”对应的判定；
- 基线交错运行；丢弃各配置首轮。

### 9.3 一致性校验（结果可信门槛）

| 校验 | 判据 |
|------|------|
| 慢路径计数 | `creates_actual == creates_expected`；随机访问解析式：
  触及数 `D ≈ M(1−(1−1/M)^N)`，`creates_expected = invalid_ratio_actual × D`
  （target 模式按集合精确计算；比例取整后以实际值为准） |
| 快路径形态 | T1 的 `instr_per_op == B0 + VAMAP_CONV_INSNS`（x86-64 strict 默认 8、fast 7；BMI2 `shrx` 变体 7/6；按构建变体断言） |
| 时延正交性 | B1 在四种时延下 ns_per_op 偏差 ≤ 1% |
| 重复稳定性 | 中位数 CV ≤ 5%，否则加跑 |

## 10. 预期结果与合理性判据

量级预期（3 GHz x86-64 参考量级，具体随微架构/主机变化，用于捕获明显异常）：

| 场景 | 预期 overhead |
|------|---------------|
| T1（0% 无效） | +5–15 cycle/op（≈2–5 ns） |
| T2（0% 无效） | +30–60 cycle/op（表项 LLC 命中） |
| T2（100%，10us） | 平均 ≈ creates×10us/N + 快路径成本 |
| T2（100%，10ms） | 总时间被创建主导：≈ creates × 10ms |
| T3–T6（0% 无效，STREAM-M） | 带宽下降 5–30% |
| T7 表项步长 1 vs 64 | 步长 64 的开销显著更高（表行预取失效） |
| T8 | 每 hop 叠加表项访问延迟，0% 与 100% 差异最大 |
| T10 | L1/L2/LLC/DRAM 四级台阶清晰 |
| T11 shared vs disjoint | shared 墙钟 ≈ latency + 交接；disjoint 近线性 |
| T15 | hoist 版开销 ≈ B0 + 边界检查，明显低于逐次版 |

异常处理：任一结果偏离预期量级 2× 以上 → 记录并复查（快路径形态、频控、表大页）。

## 11. 输出与报告

CSV schema（每行一个配置一次运行）：

```text
suite,host,date,arch,build,test_id,pattern,variant,mode(B0|B1|B2),threads,idx_mode,alias,
invalid_ratio,invalid_ratio_actual,invalid_scope,latency_ns,latency_mode,M,ops,reps,
creates_expected,creates_actual,destroys,
ns_per_op_med,ns_per_op_min,ns_per_op_max,cycles_per_op,instr_per_op,
l1m_per_op,llc_per_op,dtlb_per_op,br_per_op,minor_faults,major_faults,
slow_ns_total,slow_pct,overhead_pct,CV,notes
```

图表清单：

1. overhead 热力图：ratio（5）× latency（4），分 pattern、分 threads；
2. T10 工作集台阶曲线；T8/T14 延迟分解对比；
3. 多线程加速比曲线（private/shared × threads）；
4. T12 开销–churn 间隔曲线；T15 逐次 vs hoist 对比柱状图。

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
```

运行预算参考：`full` 档 ≈ 15 pattern × 20 组合 × 5 线程档 × reps 3–5，
重组合由 §4.4 自适应缩规模控制在每重复 ≤ 1 min 量级；CI 使用 `smoke` 档
（threads 1/4，重组合自动截断）约 20 分钟。
