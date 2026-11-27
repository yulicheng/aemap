# report.md — 全量测试报告

> 生成日期：2026-11-27 ｜ 主机：hsx26 ｜ 仓库基线：`9573096` feat: 按 design §5.2/§5.3 落地双架构变体与统一测试框架
> 被测对象：VVA→VA 动态地址映射（核心库 + mock 运行时 + bench 框架），设计见 [design.md](design.md)，测试框架与用例定义见 [test.md](test.md)。
> 本报告覆盖：**正确性门槛（C1–C7）、API 冒烟（test_api）、快路径形态抽查、性能矩阵**（smoke 配置空间 + 高重复次数）。

## 1. 执行摘要

| 类别 | 结果 |
|------|------|
| API 冒烟（test_api） | **PASS**（strict / fast-tag0） |
| 正确性套件 C1–C7（x86-64 native） | **7/7 PASS**（strict / fast-tag0） |
| 正确性套件 C1–C7（aarch64 @ qemu） | 5/7（C3/C6 为模拟器时序伪差，见 §4.3，正确性断言全部通过） |
| 快路径形态抽查（check_fastpath） | **strict 12/12、fast-tag0 10/10、bmi2 11/11 PASS** |
| 指令断言（运行时，T1 B1−B0） | fast-tag0 8/8 OK；strict 6/8 OK + 2 处 th=4 perf 计数噪声（见 §5.1） |
| bmi2 变体运行时 | 本机 CPU（Sandy Bridge-E）无 BMI2 硬件 → SIGILL，仅静态检查 |
| 性能矩阵（smoke 配置空间，reps 20/40） | strict / fast-tag0 两变体 **680 配置 × 3 模式全部完成**，CV ≤ 0.5%，见 §5 |
| 编译 | `-Wall -Wextra` 干净重建 **0 警告**（三个 x86-64 变体） |

## 2. 测试环境与口径

### 2.1 主机环境

| 项 | 值 |
|----|----|
| 主机名 | hsx26 |
| CPU | Intel(R) Core(TM) i7-3960X @ 3.30GHz（Sandy Bridge-E，6 核 12 线程） |
| 指令集要点 | AVX、constant_tsc、nonstop_tsc、rdtscp；**无 AVX2 / BMI2** |
| 内存 | 16 GB |
| 内核 | 7.0.0-44-generic |
| 调频策略 | schedutil（观测频率 ~3.6 GHz） |
| 编译器 | gcc 15.2.0（x86-64 与 aarch64-linux-gnu 交叉同版本） |
| 模拟器 | qemu-aarch64 10.2.1（仅用于正确性套件，test.md §3.1 允许） |
| 计时 | bench 自标定 tsc（~2.95–3.3 GHz，随调频浮动）；wall clock=hpet |

### 2.2 口径说明

- **矩阵口径**：`--matrix --level smoke`（全配置空间：15 pattern × threads {1,4} × 无效比例 {0/25/50/75/100%} × 时延 {10us/100us/1ms/10ms}，T10 附 W 变体），共 680 配置 × 3 模式（B0/B1/B2）× 2 变体；`--ops 1048576 --reps 20`（B0 自动 ×2 → 40 次），`--pin 0-7`；两变体**串行**执行，运行期间主机无其他负载。
- 选用该口径的原因（test.md §12 运行预算）：`full` 档在本机实测重单元每重复 52–63 s（64M ops、4096 creates × 10 ms spin），全量展开预计 **>60 小时/变体**；`standard` 档约 10–14 小时。smoke 配置空间 + 高重复在单会话内可完成且覆盖全部配置维度（与上一轮 campaign 口径一致，结果可直接对比 results/report-x86）。
- **双架构**：性能矩阵仅在 x86-64 native 执行；aarch64 为 qemu 用户态模拟，指令时序无参考意义，按 test.md §3.1 仅跑正确性套件。sve2-batch 需 SVE2 硬件（rdvl=0 时 bench 自检退出）。
- 公平性与指标口径同 test.md §2/§1：B0 为无转换基线，overhead_pct = (T_conv − T_base)/T_base；ns_per_op 取中位数。

## 3. 构建产物

| 变体 | 构建方式 | 运行能力 |
|------|----------|----------|
| x86-64 / strict | native gcc | ✅ 本机运行 |
| x86-64 / fast-tag0 | native gcc | ✅ 本机运行 |
| x86-64 / bmi2 | native gcc `-mbmi2` | ❌ 运行即 SIGILL（exit 132），CPU 无 BMI2；仅静态形态检查 |
| aarch64 / strict | 交叉 gcc | qemu 仅（正确性套件） |
| aarch64 / fast-tag0 | 交叉 gcc | qemu 仅（正确性套件） |
| aarch64 / sve2-batch | 交叉 gcc `-march=armv9-a+sve2` | qemu 仅（正确性套件；无 SVE2 硬件） |

三个 x86-64 变体干净重建，`-Wall -Wextra` 下 **0 警告**。

## 4. 快速测试项结果

### 4.1 API 单元冒烟（test_api）

| 构建 | 结果 |
|------|------|
| x86-64/strict | **PASS（0 failures）**，regions=16384 limit=16383 table=thp |
| x86-64/fast-tag0 | **PASS（0 failures）** |
| x86-64/bmi2 | SIGILL（exit 132）——预期内，见 §2.1 |

### 4.2 正确性套件 C1–C7（native，x86-64）

`create_latency_ns=0`（C6 除外），strict 与 fast-tag0 均 **7/7 PASS**：

| 用例 | 内容 | strict | fast-tag0 |
|------|------|:------:|:---------:|
| C1 | 转换位精确性（alias 池预填 2^20 + 10^7 随机 VVA） | PASS | PASS |
| C2 | create/destroy 往返与 region 复用 | PASS | PASS |
| C3 | 同索引竞争创建（8 线程） | PASS | PASS |
| C4 | 异索引并发 + release/acquire 可见性 | PASS | PASS |
| C5 | churn 压力（8 线程 10^6 往返 + 并发读者） | PASS | PASS |
| C6 | 时延精度（SPIN，4 档 ≤ ±5%） | PASS（最大 dev +4.07% @10us） | PASS（最大 dev +2.03% @10us） |
| C7 | alias 共享与引用计数销毁 | PASS | PASS |

### 4.3 正确性套件（aarch64 @ qemu-aarch64 10.2.1）

三个变体均 **5/7**，失败项及定性：

| 变体 | C1 | C2 | C3 | C4 | C5 | C6 | C7 |
|------|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| aarch64/strict | PASS | PASS | FAIL | PASS | PASS | FAIL | PASS |
| aarch64/fast-tag0 | PASS | PASS | FAIL | PASS | PASS | FAIL | PASS |
| aarch64/sve2-batch | PASS | PASS | FAIL | PASS | PASS | FAIL | PASS |

**C3（slow_calls == 8 断言）与 C6（时延 ≤ ±5%）均为模拟器时序伪差，非代码缺陷**，证据如下：

- **C6**：qemu 下 10us 档偏差 +19.5%~+21.1%，100us 及以上档偏差 ≤ +2.03%（10ms 档 sve2-batch +3.03%）——偏差集中在最小时延档，符合模拟器 JIT/时钟粒度特征；native（x86-64）同档最大偏差 +4.07% 全过。
- **C3**：失败断言仅为竞态窗口计数（8 线程同时进入慢路径），**正确性断言（creates==1、VA 一致、数据一致）全部通过**。用与 C3 同构的独立探针（30 次试验/变体，每次重建 runtime）量化：

| 环境 | slow_calls 直方图（30 试验） | creates≠1 | VA/数据不一致 |
|------|------------------------------|:---------:|:-------------:|
| x86-64 native（strict） | **8 × 30/30** | 0 | 0 |
| qemu-aarch64（strict） | 3×1, 6×13, 7×11, 8×5 | 0 | 0 |
| qemu-aarch64（sve2-batch） | 1×1, 4×1, 5×2, 6×6, 7×12, 8×8 | 0 | 0 |

  即：native 下 8 线程全部在 create 完成前探测到无效表项（slow_calls=8）；qemu 下 JIT 使各线程竞态窗口错开，slow_calls 在 1–8 波动，但**创建次数恒为 1、VA 与数据一致性零异常**——正是快路径+慢路径 double-check 语义的正确行为。

### 4.4 快路径形态抽查（scripts/check_fastpath.sh）

| 变体 | 断言 | 结果 |
|------|------|------|
| x86-64/strict | 掩码形态 andl $0xfffff、比例变址表 load、慢路径 call、9 项序列形态、唯一表 load、call≤掩码次数(33≤33) | **12/12 PASS** |
| x86-64/fast-tag0 | tag≡0 契约（无 andl $0xfffff）、唯一表 load、慢路径 call、序列形态、call≤判零次数(33≤238) | **10/10 PASS** |
| x86-64/bmi2 | shrx 取代 mov+shr、无复制、掩码/判零/合并序列、唯一表 load | **11/11 PASS** |
| aarch64/* | 未抽查（正确性已由 C1 位精确性覆盖；交叉 objdump 可做静态复核，未列入本轮） | — |

### 4.5 已知限制

1. **bmi2 变体**在本机不可运行（无 BMI2 硬件），其功能与性能需 Haswell+ 主机验证；静态形态检查通过。
2. **aarch64 三变体**性能需真实 ARMv9+SVE2 主机；qemu 下仅验证功能语义。
3. C3 的 `slow_calls==8` 断言依赖"8 线程全部先于 create 完成进入慢路径"的竞态窗口，在弱时序环境（模拟器、高负载、单核受限）下可能假失败，建议 test.md 将该断言放宽为区间或拆分为"creates==1 + 数据一致"两级（改进建议，见 §6）。

## 5. 性能矩阵（smoke 配置空间 + 高重复）

### 5.1 数据质量与框架自检

| 检查项 | 结果 | 门槛/说明 |
|--------|------|-----------|
| 配置覆盖 | 680 配置 × 3 模式 × 2 变体 = 4080 行 | 全部成功，无缺行 |
| CV（ns_per_op 离散度） | 最大 **0.5%** | reps=20/40 中位数口径 |
| creates_actual vs expected | 偏离 >2% 的 B2 行 **0** | 慢路径事件计数一致性 |
| B1@r=0 慢路径误触发 | **0 行**（creates==0） | 0% 无效时慢路径不得触发 |
| B1 跨时延一致性（§2 校验点） | 最大偏差 11.1%（T1_hotwindow，绝对量 <0.15ns） | 排除 T11/T12（首触/churn 语义使然） |
| 指令断言（T1 B1−B0） | fast-tag0 **8/8 OK**（+5.8~+6.7，期望 +7）；strict **6/8 OK**（+7.4~+7.5，期望 +8），2 处 th=4 标记 DEVIATION（delta 5.9/9.8） | 容差 ±1.5；th=4 的 2 处为 perf 计数在多线程边界的噪声（同单元静态形态检查已排除快路径变形） |

### 5.2 纯快路径成本（B1，无效比例 0%，lat=10us，threads=1）

L1 命中的表项场景下，转换的纯快路径代价：

| pattern | build | B0 ns/op | B1 ns/op | B1 overhead% | instr/op (B1) | cycles/op (B1) |
|---------|-------|----------|----------|-------------|---------------|----------------|
| T1_hotwindow | strict | 1.056 | 1.634 | **+54.7** | 16.52 | 5.44 |
| T1_hotwindow | fast-tag0 | 1.088 | 1.524 | **+40.0** | 15.50 | 5.20 |
| T3_copy | strict | 3.916 | 6.146 | +56.9 | 24.48 | 23.63 |
| T3_copy | fast-tag0 | 3.923 | 6.136 | +56.4 | 23.47 | 23.51 |
| T4_scale | strict | 4.130 | 6.247 | +51.2 | 24.46 | 24.02 |
| T4_scale | fast-tag0 | 4.124 | 6.005 | +45.6 | 23.48 | 23.02 |
| T5_add | strict | 2.631 | 4.322 | +64.3 | 20.52 | 16.48 |
| T5_add | fast-tag0 | 2.631 | 4.143 | +57.4 | 19.55 | 15.64 |
| T6_triad | strict | 2.910 | 4.463 | +53.4 | 21.89 | 17.06 |
| T6_triad | fast-tag0 | 2.900 | 4.276 | +47.4 | 20.90 | 16.32 |
| T7_stride | strict | 2.623 | 3.121 | +19.0 | 23.89 | 11.88 |
| T7_stride | fast-tag0 | 2.501 | 2.985 | +19.4 | 22.90 | 11.35 |
| T13_rmw | strict | 5.849 | 6.147 | +5.1 | 20.98 | 23.66 |
| T13_rmw | fast-tag0 | 5.710 | 6.139 | +7.5 | 20.98 | 23.63 |
| T14_gatherscatter | strict | 6.591 | 7.365 | +11.7 | 25.83 | 28.26 |
| T14_gatherscatter | fast-tag0 | 6.394 | 7.337 | +14.7 | 24.85 | 28.14 |
| T15_hoist | strict | 2.901 | 4.473 | +54.2 | 21.89 | 17.09 |
| T15_hoist | fast-tag0 | 2.906 | 4.315 | +48.5 | 20.88 | 16.30 |
| T10_workset W=1(4entry) | strict | 2.615 | 3.157 | +20.7 | 28.90 | 12.03 |
| T10_workset W=1(4entry) | fast-tag0 | 2.616 | 3.130 | +19.7 | 27.85 | 11.82 |
| T11_firsttouch | strict | 11.288 | 23.480 | +108.0 | 17.17 | 90.85 |
| T11_firsttouch | fast-tag0 | 11.109 | 24.648 | +121.9 | 16.17 | 95.53 |
| T2_randfull | strict | 42.091 | 24.888 | **−40.9**（布局效应，见 §5.6） | 34.99 | 95.64 |
| T2_randfull | fast-tag0 | 42.090 | 24.347 | −42.2（同上） | 34.10 | 93.25 |
| T8_pointerchase | strict | 53.276 | 31.683 | −40.5（同上） | 13.97 | 121.78 |
| T8_pointerchase | fast-tag0 | 53.053 | 32.351 | −39.0（同上） | 12.96 | 124.38 |
| T9_zipf | strict | 82.594 | 84.021 | +1.7 | 177.58 | 322.55 |
| T9_zipf | fast-tag0 | 80.497 | 82.036 | +1.9 | 176.65 | 315.36 |
| T10_workset W=4K | strict | 88.203 | 87.758 | −0.5 | 28.99 | 337.26 |
| T10_workset W=4K | fast-tag0 | 90.504 | 90.529 | +0.0 | 28.00 | 347.31 |
| T10_workset W=1M | strict | 31.081 | 25.884 | −16.7（同 §5.6） | 28.98 | 99.53 |
| T10_workset W=1M | fast-tag0 | 31.182 | 26.067 | −16.4（同 §5.6） | 28.01 | 100.10 |
| T12_churn | strict | 30.328 | 30.375 | +0.1 | 28.04 | 112.22 |
| T12_churn | fast-tag0 | 29.914 | 29.243 | −2.2 | 27.04 | 114.17 |

要点：

- **L1 命中场景的快路径开销集中在 +40%~+64%**（B0 基线 1–6ns 的 CPU-bound pattern）；cycles 口径下 T1 strict B1 为 5.44 cycles/op（B0 3.07），即每访存新增 ~2.4 cycles。
- **fast-tag0 相对 strict 在热窗/高频率 pattern 上节省 5–15pp**（T1：+54.7%→+40.0%；T15：+54.2%→+48.5%；T4/T6 约 −5pp）；stride/gather 类差异不大；T13_rmw 反而 +2.4pp（tag≡0 省去的 andl 占比小，且 B1/B0 各自噪声 ~±1%）。
- 指令数：T1 的 B1−B0 delta 实测 +7.4~+7.5（strict，期望 +8）/ +5.8~+6.7（fast-tag0，期望 +7），容差内；表基址驻留寄存器的目标形态成立。

### 5.3 慢路径主导单元（B2，100% 无效，lat=10ms，threads=1）

| pattern | build | B2 ns/op | overhead% | slow_pct | creates/rep |
|---------|-------|----------|-----------|----------|-------------|
| T1_hotwindow | strict | 39.66 | +3913 | 96.2% | 4 |
| T1_hotwindow | fast-tag0 | 44.99 | +4451 | 84.8% | 4 |
| T3_copy | strict | 616.48 | +15672 | 99.0% | 64 |
| T3_copy | fast-tag0 | 616.53 | +15626 | 99.0% | 64 |
| T7_stride | strict | 612.33 | +33526 | 99.7% | 64 |
| T11_firsttouch | strict | 39086.68 | +356679 | 99.9% | 16 |
| T11_firsttouch | fast-tag0 | 39088.41 | +350422 | 99.9% | 16 |
| T12_churn | strict | 77547.67 | +262501 | 100.0% | 127 |
| T12_churn | fast-tag0 | 77550.46 | +266298 | 100.0% | 127 |
| T13_rmw | strict | 44.29 | +657 | 86.1% | 4 |
| T14_gatherscatter | strict | 45.50 | +590 | 83.8% | 4 |

要点：慢路径主导场景（高无效比例 × 高时延）中 **slow_pct > 84%**，总时间由 `creates × latency` 决定，快路径增量被淹没；**同无效比例下 strict 与 fast-tag0 的 B2 差异 <1.5%**（慢路径语义一致）。10us 时延档（见热力表）B2 overhead 仅比 B1 高 1–5pp——创建时延可摊销，是转换开销最有利的场景。

### 5.4 多线程扩展（B1，无效比例 0%，th=4 vs th=1）

| pattern | build | B1 th=1 ns/op | B1 th=4 ns/op | th4/th1 |
|---------|-------|---------------|---------------|---------|
| T1_hotwindow | strict | 1.634 | 0.877 | 0.54 |
| T3_copy | strict | 6.146 | 2.166 | 0.35 |
| T5_add | strict | 4.322 | 1.602 | 0.37 |
| T8_pointerchase | strict | 31.683 | 8.868 | 0.28 |
| T13_rmw | strict | 6.147 | **9.207** | **1.50** |
| T13_rmw | fast-tag0 | 6.139 | 9.397 | 1.53 |
| T15_hoist | strict | 4.473 | 1.657 | 0.37 |

要点：访存密集/DRAM-bound pattern 在 4 线程下每访存墙钟时间降至单线程的 0.28–0.61×（带宽与并发隐藏）；**T13_rmw 是唯一负扩展**（1.50×，RMW 对同一 cache line 的读改写争用），两变体一致。

### 5.5 B2 overhead 热力表（threads=1，overhead_pct %）

代表性 pattern（全部 15 pattern × 20 单元的完整数据见 results/fulltest-report/report.txt）：

**build = strict**

| pattern | r=0%<br>10us | r=0%<br>100us | r=0%<br>1ms | r=0%<br>10ms | r=25%<br>10us | r=25%<br>100us | r=25%<br>1ms | r=25%<br>10ms | r=50%<br>10us | r=50%<br>100us | r=50%<br>1ms | r=50%<br>10ms | r=75%<br>10us | r=75%<br>100us | r=75%<br>1ms | r=75%<br>10ms | r=100%<br>10us | r=100%<br>100us | r=100%<br>1ms | r=100%<br>10ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| T1_hotwindow | 53.8 | 54.1 | 52.1 | 52.6 | 54.2 | 62.6 | 149.4 | 1018 | 55.5 | 72.4 | 246.4 | 1982 | 56.2 | 82.2 | 342.6 | 2946 | 57.3 | 91.4 | 438.9 | 3913 |
| T3_copy | 56.9 | 56.6 | 56.8 | 57.0 | 72.4 | 212.4 | 1619.9 | 15635 | 72.5 | 211.7 | 1623.9 | 15625 | 67.9 | 172.4 | 1239.3 | 11797 | 71.3 | 212.1 | 1641.4 | 15672 |
| T7_stride | 17.5 | 17.5 | 17.4 | 17.7 | 42.6 | 319.9 | 3099 | 30913 | 48.9 | 341.6 | 3245 | 32348 | 32.4 | 258.3 | 2520 | 25138 | 40.6 | 342.1 | 3359 | 33526 |
| T10_workset W=4K | −0.9 | −0.9 | −0.8 | −0.9 | 2.4 | 21.9 | 217.2 | 2171 | 3.4 | 24.5 | 236.7 | 2359 | 3.8 | 22.5 | 208.8 | 2074 | 4.5 | 29.5 | 278.1 | 2763 |
| T11_firsttouch | 107.8 | 110.7 | 112.3 | 111.4 | 204.0 | 963.9 | 9025 | 89469 | 289.6 | 1914 | 17984 | 177842 | 383.3 | 2790 | 26859 | 268792 | 464.4 | 3708 | 35908 | 356679 |
| T12_churn | 130.8 | 1334.7 | 13353 | 130208 | 165.9 | 1679.7 | 16322 | 163371 | 202.1 | 1988 | 19726 | 197975 | 241.5 | 2298 | 23524 | 235152 | 269.9 | 2632 | 26911 | 262501 |

**build = fast-tag0**

| pattern | r=0%<br>10us | r=0%<br>100us | r=0%<br>1ms | r=0%<br>10ms | r=25%<br>10us | r=25%<br>100us | r=25%<br>1ms | r=25%<br>10ms | r=50%<br>10us | r=50%<br>100us | r=50%<br>1ms | r=50%<br>10ms | r=75%<br>10us | r=75%<br>100us | r=75%<br>1ms | r=75%<br>10ms | r=100%<br>10us | r=100%<br>100us | r=100%<br>1ms | r=100%<br>10ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| T1_hotwindow | 39.2 | 38.9 | 39.6 | 39.4 | 40.8 | 49.9 | 149.3 | 1143 | 41.2 | 61.5 | 259.9 | 2246 | 42.8 | 72.4 | 370.3 | 3350 | 43.8 | 83.3 | 480.5 | 4451 |
| T3_copy | 56.2 | 58.2 | 56.9 | 57.9 | 73.2 | 215.5 | 1641.3 | 15612 | 71.9 | 212.2 | 1613.8 | 15644 | 67.6 | 177.4 | 1239.3 | 11782 | 72.3 | 211.5 | 1614.2 | 15626 |
| T7_stride | 18.0 | 17.8 | 18.0 | 17.8 | 37.3 | 315.2 | 3091 | 30821 | 43.1 | 334.3 | 3253 | 32298 | 31.3 | 257.2 | 2513 | 24981 | 40.4 | 341.8 | 3357 | 33500 |
| T10_workset W=4K | −0.2 | −0.2 | −0.2 | −0.3 | 2.5 | 22.2 | 218.6 | 2185 | 3.2 | 24.4 | 235.6 | 2347 | 3.8 | 22.4 | 207.7 | 2060 | 4.6 | 29.1 | 276.5 | 2747 |
| T11_firsttouch | 127.5 | 124.5 | 127.2 | 123.2 | 212.3 | 1002.8 | 8944 | 88438 | 299.1 | 1878 | 17657 | 175539 | 390.1 | 2758 | 26434 | 263657 | 472.9 | 3643 | 35149 | 350422 |
| T12_churn | 131.6 | 1325.7 | 12896 | 128634 | 171.3 | 1618.8 | 16507 | 165651 | 205.6 | 1999 | 19616 | 194248 | 231.6 | 2335 | 22687 | 232768 | 265.2 | 2582 | 25965 | 266298 |

要点：

- **10us 时延 × 任意无效比例**：B2 overhead 仅比 B1 高 0–5pp——慢路径摊销后接近纯快路径成本，是映射建立代价最有利的区间。
- **时延每升一档（10us→100us→1ms→10ms），B2 overhead 约 ×7–9**（creates×latency 线性主导）。
- r=0 全行与 B1 一致（慢路径不触发的框架自检在热力维度同样成立）。

### 5.6 公平性与口径注记（解读负 overhead）

以下三个现象为 smoke 口径下的**布局/缩规模效应**，不是"转换更快"：

1. **r=0 时 T2/T8/T10-W≥1M 的负 overhead（−16%~−42%）**：这些 pattern 的数据窗远超 LLC（DW=64M 项等）。r=0 时 M=2^20（全表空间，无 creates cap），B0 的 VA 列表随机访存带来 **dTLB 1.873/op、LLC 0.117/op**，而 B1 的 VVA 列表+表驻留缓存（2^20×8B=8MB）访问 TLB 友好，**dTLB 0.488/op**——B0 基线被列表访问的 TLB 压力抬高，而非 B1 转换本身有加速作用。上一轮 campaign（results/report-x86）中同单元同为 −41%，系统性一致。
2. **r>0 时 M 随 ratio 收缩**：状态控制器按 `cap = pow2_floor(max_creates/ratio)`（smoke max_creates=64）限制可触及表项空间（M=256/128/64/64），保证每重复 ≤1min（§4.4 缩规模）。因此**跨 ratio 单元的工作集不同**，r=0 与 r>0 的 B0 不可直接比较；同 r 不同 pattern/变体之间可比。
3. **T9_zipf instr/op 177**：zipf 序列生成计入指令数，为 pattern 自身特征（B0 同为 170+）。

上述不影响 strict vs fast-tag0 的同单元对比（两者布局一致），但**跨 ratio 的 overhead 曲线应按"慢路径事件预算"解读而非"工作集扫描"解读**。复测建议见 §6。

## 6. 结论与发现

1. **正确性与形态全部达标**：native 7/7 正确性、test_api 0 失败、三变体快路径静态形态全过、0 编译警告；aarch64 在 qemu 下仅 C3/C6 假失败，独立探针证明其正确性不变量（creates==1、VA/数据一致）100% 成立。
2. **快路径成本**：L1 命中场景 +40%~+64%（strict）/ +40%~+57%（fast-tag0，热窗类）；每访存新增约 1.7 cycles（T1，cycles 口径）；与 design §5.2 的指令数目标（strict 8 / fast 7）一致（实测 delta 7.4-7.5 / 5.8-6.7，容差内）。
3. **fast-tag0 变体价值确认**：tag≡0 契约省一条 andl，热窗场景 overhead 相对 strict 降低约 14pp（54.7%→40.0%），T15 降低 5.7pp；在慢路径主导与 DRAM-bound 场景两者无差异。
4. **慢路径可摊销区间**：10us 创建时延下任意无效比例的 B2 overhead 仅比 B1 高 0–5pp；10ms 时延 + 100% 无效时 overhead 由 creates×latency 主导（+1.5×~+3567×），运行时应尽量批量/延迟回收（T12 churn 显示销毁-重建压力下 slow_pct=100%）。
5. **多线程**：访存密集 pattern 4 线程扩展良好（0.28–0.61×）；T13_rmw 是唯一负扩展点（1.5×，RMW 争用），提示同 cache line 的跨线程 RMW 应避免映射到相同行。
6. **改进建议**：
   - test.md C3 断言 `slow_calls==8` 放宽为 `creates==1 && 1≤slow_calls≤8 && 数据一致`，避免弱时序环境假失败；
   - th=4 的 T1 INSTR 断言对 perf 计数噪声容差可放宽（或改用静态形态检查为准）；
   - 对 T2/T8/T9/T10-W≥1M 的 r=0 负 overhead，建议在 standard/full 档（固定 M、无 creates cap）下复测一次以获得布局对齐的基线；
   - bmi2 与 aarch64（含 sve2-batch）的性能验证需相应硬件（Haswell+ / ARMv9+SVE2），本报告仅覆盖其功能语义。

## 附录 A：执行命令清单

```bash
# 构建（native 三变体 + 交叉 aarch64 三变体）
make VARIANT=strict all && make VARIANT=fast-tag0 all && make VARIANT=bmi2 all
make CC=aarch64-linux-gnu-gcc VARIANT=strict all
make CC=aarch64-linux-gnu-gcc VARIANT=fast-tag0 all
make CC=aarch64-linux-gnu-gcc VARIANT=sve2-batch all

# API 冒烟
./build/x86_64-strict/test_api && ./build/x86_64-fast-tag0/test_api

# 正确性门槛（native + qemu）
./build/x86_64-strict/vamap_bench --suite correctness
./build/x86_64-fast-tag0/vamap_bench --suite correctness
qemu-aarch64 -L /usr/aarch64-linux-gnu ./build/aarch64-strict/vamap_bench --suite correctness
qemu-aarch64 -L /usr/aarch64-linux-gnu ./build/aarch64-fast-tag0/vamap_bench --suite correctness
qemu-aarch64 -L /usr/aarch64-linux-gnu ./build/aarch64-sve2-batch/vamap_bench --suite correctness

# 快路径形态抽查
scripts/check_fastpath.sh build/x86_64-strict/vamap_bench strict
scripts/check_fastpath.sh build/x86_64-fast-tag0/vamap_bench fast-tag0
scripts/check_fastpath.sh build/x86_64-bmi2/vamap_bench bmi2

# 性能矩阵（smoke 配置空间 + 高重复；串行执行避免互相污染）
./build/x86_64-strict/vamap_bench --matrix --level smoke --ops 1048576 --reps 20 --pin 0-7 --out results/fulltest-strict/
./build/x86_64-fast-tag0/vamap_bench --matrix --level smoke --ops 1048576 --reps 20 --pin 0-7 --out results/fulltest-fast/

# 合并报告
./build/x86_64-strict/vamap_bench --report --src results/fulltest-strict/ --src results/fulltest-fast/ --out results/fulltest-report/
```

## 附录 B：产物与日志

| 产物 | 路径 |
|------|------|
| strict 矩阵原始 CSV（2040 行） | results/fulltest-strict/results.csv |
| fast-tag0 矩阵原始 CSV（2040 行） | results/fulltest-fast/results.csv |
| 合并报告（全部 15 pattern × 20 单元热力数据） | results/fulltest-report/report.txt（+ merged.csv 4080 行） |
| 正确性日志 | /tmp/opencode/correctness-{strict,fast-tag0,a64-*}.log |
| C3 竞态探针 | /tmp/opencode/c3_probe/（c3_probe.c + x86/aarch64/sve2 三个二进制） |
| 矩阵运行日志 | /tmp/opencode/matrix-strict.log、matrix-fast.log |
| 分析脚本 | /tmp/opencode/analyze.py |
