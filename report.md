# VVA→VA 动态地址映射 — x86_64 / AArch64 双架构测试报告

> 测试日期:2026-11-25(PMU 开启后第 2 轮全量重测;第 1 轮为 rdtsc 回退时代,数据已全部作废)
> 测试对象:软件 VVA→VA 转换(见 `design.md`)及其 overhead 量化测试框架(见 `test.md`)
> 测试方式:**x86_64 原生运行,PMU 全指标**(cycles/instructions/L1D-miss/LLC-miss/dTLB-miss);**AArch64 交叉编译 + qemu-user 用户态仿真**(功能等价,性能为仿真参考值)

---

## 1. 执行摘要

| 项目 | x86_64(原生,PMU) | AArch64(qemu-user 仿真) |
|---|---|---|
| C0 功能 gate(10⁶ 随机 LD/ST + tag/对齐/哨兵/公平性断言) | **PASS** | **PASS** |
| Δins/op 契约(design §8.1) | **动态 PMU 校验 46/46 PASS**(±0.02)+ codegen 静态核对双证据 | qemu 无 PMU → **codegen 静态核对 PASS**(strict=4);clang 交叉 `_Static_assert` 4 变体通过 |
| C4 K 阶梯(K=8→2²⁰) | 阶梯清晰:+1.48 → +18.97 c/op;PMU 归因:**K=1M 段主因 dTLB(Δ+0.75/op)而非 LLC** | 平缓:+3.47 → +6.44 c/op(仿真无真实缓存) |
| C13 变体矩阵 | 9 变体(含 clang/abs)OK,v3 仅编译 | 6 变体 OK(abs/v3 为 x86 专属) |
| C2 热表下界(验收 ≤3 c/op) | **PASS**(+1.07~+1.75,fast 最优) | 超标(+2.75~+3.52,仿真固有开销,需真机复测) |
| C6 冷表依赖链 | **+228.8 c/op**,落于 120–250c 预期区间;Δllcm=+1.00/Δl1m=+1.01 逐跳精确 | +3.65(无效:仿真无 DRAM 延迟模型) |
| IPC 证据 | 吞吐场景 conv IPC **高于** direct(1.8→2.4,转换 ALU 填充访存空隙);延迟场景坍缩(0.25→0.13) | n/a(qemu 计时为 cntvct 墙钟) |

**核心结论**:
1. **功能正确性在两架构全部通过**;x86 侧指令数契约首次获得**动态闭环**(PMU Δins 与 §8.1 逐变体一致:strict=5/fast=4/A4-strict=6),静态反汇编为第二证据。
2. **x86 热表转换开销 ≈ +1.1~+1.5 c/op**——5 条转换指令在表 L1 命中时约 1 条的有效代价(乱序隐藏率 ≈70%);PMU IPC 数据给出直接证据:conv 侧 IPC 从 1.8 升至 2.4,即转换 ALU 指令填充了访存等待空隙。
3. **PMU 归因修正**:C4@K=1M(+19 c/op)并非 LLC 容量问题(8MB 表 < 15MB LLC,Δllcm≈+0.01),而是**表侧 dTLB:Δ+0.748/op,与 STLB 容量模型 1−512/2048=0.75 精确吻合**——印证 design §4.13"表须 hugepage 映射"论证;C5 全冷才是 LLC/DRAM 场景(Δllcm+0.44、Δdtlb+0.75、IPC 2.40→0.34)。
4. **A4(4B 表项)编码在冷表场景显著优于 B8**:x86 冷 Δ +11.66 vs +18.97(−38%);ARM 同向(B8 +8.87 vs A4 +6.44)。热表场景差异 ≤0.3 c/op。
5. **AArch64 结果为 qemu-user 仿真值**:功能有效,但 C4 阶梯、C5、C6-cold、C12 等缓存/TLB 敏感用例在仿真下**不成立**,设计裁决以 x86 原生数据为准,待真机复测。

---

## 2. 测试环境

### 2.1 x86_64(原生)

| 项 | 值 |
|---|---|
| CPU | Intel Core i7-3960X @ 3.30GHz(Sandy Bridge-E,6C/12T,LLC 15MB,短循环实测 boost ~3.9GHz) |
| OS / 内核 | Ubuntu 26.04.1 / 7.0.0-43-generic |
| 编译器 | gcc 15.2.0(默认变体)、clang 21.1.8(clang 变体) |
| perf_event_paranoid | **1 → PMU 可用**(cycles/ins/L1D-miss/LLC-miss/dTLB-miss 全部生效,组内 ENABLE/DISABLE 同步,逐 fd 单独读) |
| 计时 | PMU cycles(主);`lfence;rdtsc` 仅作 freq 换算参考 |
| clflush / BMI1 / BMI2 | 支持 / 无 / 无 → **v3 变体仅编译验证**(运行会 SIGILL) |
| THP | `always [madvise] never` |
| 变体基线 | `gcc-strict-pinned`(ENC=B8,TAG=strict,BASE=pinned,-ffixed-r15,W=16) |

### 2.2 AArch64(交叉编译 + qemu-user 仿真)

| 项 | 值 |
|---|---|
| 交叉编译器 | aarch64-linux-gnu-gcc 15.2.0(`-ffixed-x27` pinned;ENC=A4 默认) |
| 仿真器 | qemu-aarch64 10.2.1(Debian 1:10.2.1+ds-1ubuntu3.3) |
| 计时 | `cntvct_el0` 回退(§3.1:固定频率架构定时器,**仅墙钟近似**,非真实 CPU cycles;qemu TCG 下 PMU 不可用) |
| 变体基线 | `gcc-strict-pinned`(ENC=A4,TAG=strict,BASE=pinned,W=16) |
| 静态验证 | clang 21.1.8 `--target=aarch64-linux-gnu -ffreestanding` 交叉编译 4 变体(含 §8.1 指令数 `_Static_assert`) |

### 2.3 测试协议(两架构一致)

- 每配置 2 次预热 + **30 次重复**,direct/conv ABBA 交错成对做差,报告中位数 ± MAD;
- 同一转换函数预生成地址列表,direct 与 conv 访问完全相同的物理 cache line(§1.1 公平性);
- 内核双实例(`kernel_defs.h` 以 `A2P` 宏实例化两份),同一编译单元、同一 `-O2 -fno-tree-vectorize -fno-tree-slp-vectorize -fno-unroll-loops`(双实例**逐指令对称**前提,见 §2.4-④);
- 结果异或累积进 `volatile g_sink` 防 DCE。

### 2.4 本机内核 PMU 兼容性 quirk 与 harness 修复(本轮新增,供复现者理解)

第 1 轮测试在 paranoid=4 下回退墙钟;本轮开启 PMU(paranoid=1)后,通过 8 个独立探针程序(`/tmp/opencode/pmu_probe*.c`)定位了本机 7.0.0-43 内核与上游 perf ABI 的三处差异,并修复 harness:

1. **组读布局差异**:带 `PERF_FORMAT_TOTAL_TIME_*` 的组读,本内核把 `time_enabled/running` 排在事件对**之前**(`[nr, ena, run, {val,id}×nr]`,上游在尾部)。harness 改为**逐 fd 单独读**(布局 `[val, ena, run(, id)]` 两代内核一致),组同步关系仍由 open 的 gfd 保持;缩放校正按各自事件 ena/run。
2. **HW_CACHE 编码字段反排**:本内核 `config = result<<16 | op<<8 | cache_id`(上游 `cache_id<<16 | op<<8 | result`)。harness 以 **LL-miss 事件做一次性 ABI 探测**选定编码(不能用 L1D 判别:l1d 的 std 编码 0x00001 在反排解码下恰为合法的 L1I-access,会静默打开错误事件——本轮曾因此 Δl1m 恒 0)。附带发现:反排语义下 id=0(L1D-miss)事件计数口径偏大(合成随机负载 ~1.8/op,超物理上界 1.0),**Δ 方向与量级仍可用**,跨内核绝对可比性存疑(§6-3)。
3. **Δins/op 校验容差**:scale = time_enabled/time_running 含 reset→enable 的纳秒级调度间隙,浮点严格相等被 5×ε 误判;改为 **±0.02 条/op 容差**(真实 codegen 差异 ≥1 条 ≫ 容差)。
4. **双实例展开不对称**(Makefile):clang -O2 将 direct 实例按 4× 展开、conv 实例因 volatile asm 保持标量,Δins 被灌入 ~2.25 条/op 循环开销(实测 7.25 = 5+2.25);依 design "双实例对称性"原则在内核编译加 `-fno-unroll-loops`(gcc -O2 本不展开该循环,codegen 无变化)。另:C11-RMW 为**每访问两次 A2P**,不适用 §8.1 单转换契约,实测 Δins=11.000 = 2×5+1(第二次 A2P 重载 VVA,反汇编逐条吻合),已随 CSV 记录并标注 n/a。

修复后交叉验证:c2 Δins=5.000 ✓;c6-cold Δllcm=+1.00、Δl1m=+1.01(每跳恰好 1 条表线)✓;c4@1M Δdtlb=+0.748 ≈ STLB 容量模型 0.75 ✓。

---

## 3. 功能测试结果(C0 差分正确性 gate)

### 3.1 断言内容与结果

C0 覆盖(test.md §4.1):随机 10⁶ 次 LD/ST 数值一致性、strict 模式 tag 不变性(随机 12bit tag 扰动)、fast 模式 tag≡0 契约、entry0 哨兵、W 上限断言、转换后指针对齐、direct/conv 地址列表逐元素公平性断言。

| 构建变体 | x86_64(原生) | AArch64(qemu) |
|---|---|---|
| default(gcc-strict-pinned) | PASS | PASS |
| fast | PASS | PASS |
| enc-swap(x86:A4 / ARM:B8) | PASS | PASS |
| mat | PASS | PASS |
| abs(仅 x86) | PASS | — |
| pure | PASS | PASS |
| seg | PASS | PASS |
| clang | PASS | — |

> 运行日志中的 `vva_map_set: 非法参数 ... abort` 为 C0 的**负向测试路径**(故意传入非法 idx/H 验证断言),非缺陷。

### 3.2 Δins/op 指令数契约(§8.1)——动态 PMU + 静态 codegen 双证据

**证据一(动态,PMU)**:x86 侧全部单转换循环用例的 Δins/op 硬校验 **46/46 PASS、0 FAIL**(容差 ±0.02,含 9 个 C4 K 档、C1/C3/C7/C9/C10/C11-r/w/C12×2/C5 与 6 个 c13 变体 ×4 档):

| 变体 | 构建期望(§8.1) | Δins 实测(c2 代表值) | 结论 |
|---|---|---|---|
| gcc-strict-pinned / default / mat / abs / clang(B8-strict) | 5 | 5.000 | PASS |
| fast(B8-fast) | 4 | 4.000 | PASS |
| enc-swap(A4-strict) | 6 | 6.000 | PASS |
| pure / seg(分支型/编译器生成) | — | n/a(契约仅定义于手写单转换循环) | 不适用 |
| C11-RMW(双转换) | — | 11.000 = 2×5+1(VVA 重载,反汇编吻合) | 记录在案 |

n/a 共 16 行:C11-RMW、C6(指针追逐非循环读)、C8(块拷贝)、C14(多流)、pure/seg——均为非单转换循环内核,契约不覆盖,以静态核对兜底。

**证据二(静态,反汇编)**:`make codegen` 存档与 clang 交叉 `_Static_assert`(bench/arm_smoke.c)复核:

| 架构 | 编码 | tag | 文档期望(§8.1) | 实测 | 结论 |
|---|---|---|---|---|---|
| x86_64 | B8 | strict | 5(mov/shr/and/mov/or) | 5 ✓(动态+静态双证) | PASS |
| x86_64 | B8 | fast | 4(省 and 掩码) | 4 ✓ | PASS |
| x86_64 | A4(enc-swap) | strict | 6(+shl) | 6 ✓ | PASS |
| x86_64 | clang(B8-strict,base=mat) | strict | 5 | 5 ✓(加 `-fno-unroll-loops` 后对称,见 §2.4-④) | PASS |
| AArch64 | A4 | strict | 4(ubfx/ldr/ubfx/orr) | 4 ✓(qemu 无 PMU,静态核对) | PASS |
| AArch64 | A4 | fast | 3 | clang 交叉 `_Static_assert` 通过 | PASS |
| AArch64 | B8(enc-swap) | strict | 4 | 编译 + C0 通过 | PASS |

AArch64 strict 转换循环实测 codegen(`k_read_c` 内层,x27 为 pinned 表基址):

```asm
ldr   x2, [x0, x3, lsl #3]      // 读 VVA
ubfx  x6, x2, #32, #20          // ① 提取 idx
ldr   w7, [x27, x6, lsl #2]     // ② 查表(A4:4B 表项,x27 pinned 基址)
ubfx  x8, x2, #0, #32           // ③ 提取 offset
orr   x5, x8, x7, lsl #32       // ④ 合并 → VA(转换序列共 4 条)
ldr   w2, [x5]                  // 数据访问
```

另:`arm_smoke.c` clang 交叉编译 4 变体(strict-pinned / fast-pinned / enc-swap-mat / strict-seg)全部通过,产物存档于 `results/arm_smoke_*.o`。

---

## 4. 性能测试结果

> 指标定义:`Δc/op = (cycles_conv − cycles_direct) / n_ops`(PMU cycles);`IPC = instructions / cycles`;`Δl1m/op、Δllcm/op、Δdtlb/op` 同理为 miss 计数差。

### 4.1 C4 随机索引 K 阶梯(核心曲线,表足迹穿越 L1→L2→LLC/STLB)

`gcc-strict-pinned`,reps=30,多对一映射解耦数据集;PMU miss 指标同步给出归因:

| K | 表足迹(B8) | Δc/op | Δl1m/op | Δllcm/op | Δdtlb/op | conv IPC | 归因 |
|---|---|---|---|---|---|---|---|
| 8 | 64B | +1.48±0.03 | +0.000 | −0.031* | +0.000 | 2.33 | 表 L1 驻留,纯指令代价 |
| 64 | 512B | +1.48±0.09 | +0.000 | −0.038* | +0.000 | 2.34 | 同上 |
| 256 | 2KB | +1.45±0.07 | +0.001 | −0.036* | +0.000 | 2.35 | 同上 |
| 1024 | 8KB | +1.45±0.09 | +0.001 | −0.039* | +0.000 | 2.36 | 同上 |
| 4096 | 32KB(≈L1 界) | +1.71±0.10 | +0.301 | −0.036* | +0.000 | 2.20 | 表开始挤占 L1 |
| 16384 | 128KB | +2.45±0.43 | +0.817 | −0.016 | +0.000 | 2.13 | 表 L1 全 miss→L2 |
| 65536 | 512KB | +5.66±0.99 | +0.960 | −0.007 | +0.000 | 1.27 | 表走 L2/LLC |
| 262144 | 2MB | +5.61±0.01 | +1.002 | +0.000 | +0.007 | 1.34 | 表 LLC 驻留 |
| 1048576 | 8MB | +18.97±1.13 | +1.389 | +0.009 | **+0.748** | 0.46 | **表 dTLB(页表遍历)** |

\* 热表段 Δllcm 为 −0.01~−0.04 的小量,处于复用缩放噪声区(方向为负、无物理意义),不解读。

- **热段(K≤1024)Δ≈+1.45~1.48 c/op**:转换序列 5 条指令中约 1 条的有效代价,conv IPC(2.33~2.36)> direct(1.78)——转换 ALU 填充了访存空隙的直接证据。
- **K=1M 段的主因是 dTLB 而非 LLC**:8MB 表仅占 15MB LLC 的一部分(Δllcm≈+0.01),但 2048 个 4KB 页远超 Sandy Bridge L2 STLB(512 项),随机访问的理论 miss 率 1−512/2048=**0.75**,实测 Δdtlb=+0.748 **分毫不差**;页表遍历延迟(~25c/次)× 0.75 ≈ +19 c/op,与实测 Δc/op 吻合。**这印证 design §4.13"表须 hugepage 映射"的论证**,也修正了第 1 轮(rdtsc)报告中"混合 DRAM"的猜测。
- **C4b(独立 H,数据 miss 叠加)**:K=4096 时 Δ=+5.39±0.02(direct 27.22 → conv 32.60);数据侧 miss 保持不变(llcm 0.400→0.399/op、dtlb 0.875→0.884/op),唯一增量是表访问自身的 l1m 1.691→2.479/op(+0.79)——表访问与数据访问 **miss 相互独立**,Δ+5.39 即一次 L1 命中表访问插入关键路径的代价(direct IPC 0.18,已高度串行)。
- **C5 表全冷(每轮 flush)**:Δ=+27.53±0.19;conv 侧 l1m 1.515/op、llcm 0.443/op、dtlb 0.752/op(IPC 2.40→0.34)。吞吐型场景下乱序执行(MLP)隐藏了大部分 DRAM 延迟;全额延迟由 C6 依赖链暴露(见下)。
- **C6 指针追逐**:热表 Δ=+9.91(完整转换关键路径,验收 ≤10 PASS);**冷表 Δ=+228.79±0.34**——落在"一次表线 DRAM 延迟 120~250c"预期区间,且 **Δllcm=+1.00、Δl1m=+1.01 逐跳精确**(每跳恰好 1 条表线自 DRAM 回取),与 C5 互为校验 ✓。冷表 conv IPC 仅 0.13,全额延迟无任何隐藏。
- AArch64 仿真下阶梯平缓(+3.47→+6.44)、C5=+6.64、C6 冷=+3.65:**qemu-user 无真实 cache/TLB/DRAM 延迟模型,这些缓存敏感用例的绝对值无效**,仅证明功能路径正确执行。

### 4.2 C2 相近 VVA 热点(必测,最优下界)

表项 1~8 个且常驻 L1,窗口 8~64B 反复访问。验收标准:热表吞吐 Δ≤3 c/op(两架构)。

| 变体 | x86_64 Δc/op | Δins/op | conv IPC | 验收 | AArch64(qemu) Δc/op |
|---|---|---|---|---|---|
| fast | **+1.07±0.07** | 4.000 ✓ | 2.32 | PASS | +3.52±0.03 |
| clang | +1.38±0.04 | 5.000 ✓ | 2.37 | PASS | — |
| gcc-strict-pinned(基准) | +1.45±0.10 | 5.000 ✓ | 2.35 | PASS | +3.45±0.04 |
| mat | +1.47±0.06 | 5.000 ✓ | 2.34 | PASS | +3.43±0.04 |
| pure | +1.48±0.04 | 5.000(实测,无契约) | 2.34 | PASS | +2.75±0.05 |
| abs | +1.49±0.01 | 5.000 ✓ | 2.33 | PASS | — |
| default | +1.52±0.03 | 5.000 ✓ | 2.31 | PASS | +3.46±0.03 |
| seg | +1.72±0.04 | n/a(段查表 +8 条) | 2.88 | PASS | +3.35±0.04 |
| enc-swap(A4) | +1.75±0.08 | 6.000 ✓ | 2.42 | PASS | +3.29±0.03(B8) |

- **x86 全变体 PASS**:热表下界 +1.1~+1.8 c/op;Δins 全部与 §8.1 逐变体一致——**序列越短 Δ 越小**(fast 4 条 +1.07 < strict 5 条 +1.45~1.52 < A4 6 条 +1.75),与指令数严格单调对应,无一条指令的额外开销逃过计量。
- ARM 仿真值(+2.8~+3.5)含 qemu 解释执行固有开销(pure 变体编译器生成代码反而最低,提示仿真对"手写短序列"不公平),**不可作为真机结论**。

### 4.3 其余用例(x86_64 原生,gcc-strict-pinned,PMU 全指标)

| 用例 | 配置 | Δc/op | IPC d→c | Δl1m/op | 备注 |
|---|---|---|---|---|---|
| C1 顺序读 | 4MB 流 | +1.25 | 1.73→2.42 | +0.000 | 表 1 项热;转换 ALU 填入流空隙 |
| C1 顺序写 | 4MB 流 | +0.68 | 2.05→2.65 | +0.000 | 写路径转换几乎全隐藏 |
| C3 段内随机 | span=64K | +1.26 | 1.49→2.16 | +0.004 | 与 C1 接近:表热时数据 pattern 无关 ✓ |
| C7 跨段步长 | stride=1,K=64K | +1.53 | 1.79→2.31 | +0.125 | 表顺序流被预取器部分隐藏 |
| C9 gather/scatter | K=4K,win=64K | +2.21 | 1.72→1.96 | +0.748 | 稀疏 miss 部分被 MLP 遮蔽 |
| C10 Zipf | s=0.9,K=64K | +4.05 | 2.15→1.57 | +0.517 | 介于 C2 与 C4 同足迹之间 ✓ |
| C11 读 | 1M span | +0.53±2.45 | 0.63→1.18 | +0.000 | Δ 小,MAD 大因宿主波动(§5 统计行) |
| C11 RMW | 1M span | +2.37±1.57 | 0.76→1.47 | +0.000 | 双转换(2×5+1 条,§2.4-④) |
| C11 写 | 1M span | +0.24±1.67 | 1.13→1.60 | +0.000 | 读/写对称性受波动限制 |
| C12 TLB(4K 页) | 256MB 足迹 | +0.24 | 0.08→0.16 | +0.000 | DRAM 受限(llcm 1.077/op 两侧相同),转换增量不可见 |
| C12 TLB(2M 页) | 256MB 足迹 | +2.43 | 0.13→0.25 | +0.000 | hugepage 消除数据侧 dTLB 噪声后 Δ 显形 |
| C14 daxpy | n=1M | +1.43 | 1.85→2.79 | +0.000 | 2 条流 + ALU 空隙(conv IPC 最高之一) |
| C14 matmul 内层 | n=192 | +2.10 | 2.45→2.67 | +0.000 | 3 条流 + 复用 |

AArch64(qemu)对应值 +0.9~+9.9 c/op 不等,其中 C11-w 与 C12(+9.9)为 qemu 对 store/大页路径的仿真偏差,无真机参考价值(详见 §6 局限)。

### 4.4 C8 拷贝带宽

16MB memcpy 型内核,频率换算带宽(x86 以运行时 cpufreq ~3.9GHz 换算;ARM 值为仿真参考,其 freq 列为宿主频率、绝对值无意义,仅 Δ% 有效):

| 变体 | 模式 | x86 direct | x86 conv | x86 Δ% | ARM(qemu) Δ% |
|---|---|---|---|---|---|
| 块级转换(64B 块) | blk | 5.60 GB/s | 5.36 GB/s | **−4.4%** | −5.1% |
| 非临时写(64B 块) | nt | 1.38 GB/s | 1.38 GB/s | **−0.0%** | −5.0% |
| 每字转换(4B,上界) | word | 2.43 GB/s | 1.91 GB/s | −21.1% | −42.8% |

验收(块级热表 Δ%≤5)**PASS**:块级摊薄后转换开销 −4.4%,NT store 与写路径完全重叠(−0.0%)。每字转换即每访存转换的最坏上界。

### 4.5 C13 变体矩阵排名(热 = c2;冷 = c4@K=1M)

变体状态:x86 九变体全部 OK(v3 因 CPU 无 BMI1 仅编译验证);ARM 六变体 OK;pf/gs 变体按 run_all.sh 声明 N/A。全部变体 Δins 硬校验 PASS 或按契约 n/a(§3.2)。

**x86_64(原生,绝对值有效)**

| 编译器/变体 | 热 Δc/op(c2) | 冷 Δc/op(c4@1M) |
|---|---|---|
| fast | **+1.07** | +19.61 |
| clang | +1.38 | +18.47 |
| gcc-strict-pinned(基准) | +1.45 | +18.97 |
| mat | +1.47 | +19.56 |
| pure | +1.48 | +21.34 |
| abs | +1.49 | +18.59 |
| default | +1.52 | +18.91 |
| seg | +1.72 | +20.83 |
| enc-swap(A4) | +1.75 | **+11.66** |

**AArch64(qemu,相对排序仅供参考)**

| 编译器/变体 | 热 Δc/op(c2) | 冷 Δc/op(c4@1M) |
|---|---|---|
| pure | +2.75 | +5.69 |
| enc-swap(B8) | +3.29 | +8.87 |
| seg | +3.35 | +10.76 |
| mat | +3.43 | +6.52 |
| gcc-strict-pinned/default(A4) | +3.45 / +3.46 | +6.44 / +6.45 |
| fast | +3.52 | +6.76 |

**跨架构发现**:
1. **热路径**:x86 上 fast(4 条)> 全部 strict(5 条)> enc-swap(A4,6 条),Δ 与指令数严格单调;pinned/mat/abs/clang 差异 ≤0.07 c/op——基址策略在吞吐场景不敏感,寄存器驻留的价值在 C6 依赖链等延迟场景。手写序列 vs pure:热表差异 ≤0.03 c/op,**gcc -O2 自动生成质量已与手写持平**;但 cold 场景 pure 最差(+21.34,无 asm 防 CSE 的寄存器分配差异)。
2. **冷路径:A4 编码(4B 表项)是冷表胜负手**——表足迹减半使 x86 冷 Δ 从 +18.97 降至 +11.66(−38%),Δdtlb 从 +0.748 回落至 +0.499(A4 表 4MB = 1024 页,理论 miss 1−512/1024 = 0.5,**STLB 容量模型再次精确吻合**);ARM 同向(B8 +8.87 vs A4 +6.44)。两架构方向一致:**对大 K 冷表负载建议 A4,热表负载 B8/A4 均可**。
3. seg(段缓存)在冷表场景反而最差(+20.83 / +10.76):额外查一级缓存在 miss 场景放大访存次数,仅在段粘性极高时才可能收益,与 design §6 预期一致。
4. AArch64 上 fast 未体现优势(+3.52):qemu 指令级时序失真,待真机复测。

---

## 5. 验收标准核对(test.md §6)

| 检查项 | 标准 | x86_64 实测 | AArch64 实测 | 结论 |
|---|---|---|---|---|
| C0 | 全部断言通过,strict tag 不变性 100% | PASS(9 变体) | PASS(6 变体 + clang 静态 4 变体) | **PASS** |
| Δins/op | 与 §8.1 一致(x86:5/4,A4:6;ARM:4/3) | **动态 PMU 46/46 PASS**(±0.02)+ 静态双证;C11-RMW=11=2×5+1 记录在案 | codegen 静态 4 ✓ + `_Static_assert`(qemu 无 PMU) | **PASS(双证据)** |
| C2 表全热吞吐 | Δ≤3 c/op | +1.07~+1.75 | +2.75~+3.52(qemu) | **x86 PASS;ARM 待真机** |
| C2/依赖链 | Δ≤10 c/op(x86) | C6-hot +9.91 | +4.17(qemu) | **x86 PASS** |
| C4 K≤4096 | Δ≤3 c/op | +1.45~+1.71 | +3.45~+3.53(qemu) | **x86 PASS;ARM 待真机** |
| C4 K=1M / C5 全冷 | ≈ +1 次表线访存延迟 | C5 +27.5(llcm+0.44/dtlb+0.75,MLP 隐藏);C6 冷链 +228.8 落于 120~250c,Δllcm=+1.00 逐跳精确 | 仿真无效 | **PASS(C5/C6 互证)** |
| C8 块级转换 | 热表 Δ%≤5% | −4.4%(nt −0.0%) | −5.1%(qemu) | **x86 PASS** |
| C14 计算内核 | Δ 显著低于 C1,目标 ≤1 c/op | daxpy +1.43 / matmul +2.10(C1=+1.25) | +3.02(qemu) | **偏差**:转换指令未完全塞入访存空隙,记录偏差待真机复核 |
| 统计稳定性 | 90% 配置 MAD/median ≤5% | 核心配置 MAD≤0.2 c/op;不稳定集中于 c4@64K(~±1)/c4@1M(±1.1)与 Δ<1 c/op 的小差值行(c11 三档) | 核心配置 MAD≤0.1 c/op;C11-w / C6-cold 为 qemu 伪影 | **部分达成**(小 Δ 行 MAD/\|Δ\| 天然偏大,核心结论行稳定) |

---

## 6. 局限性与后续工作

1. **~~PMU 不可用~~(已解决)**:本轮 paranoid=1 下 PMU 全指标生效,并完成 §2.4 所列内核兼容性修复;遗留一个事件口径问题——本内核反排 HW_CACHE 编码下 id=0(L1D-miss)计数偏大(合成随机探针 ~1.8/op,超物理上界),**同架构内 Δ 方向/量级可用(c6-cold Δl1m=+1.01 精确),跨内核绝对比较需以上游内核复核事件语义**。L2-miss(raw 事件)未纳入本轮(微架构 umask 因核而异,接口已留 `--raw-l2m`)。
2. **AArch64 为 qemu-user 仿真**:
   - 功能(语义正确性、指令数契约、变体构建矩阵)验证有效;
   - 性能绝对值不可比真实硬件(qemu 逐条翻译执行、无 cache/TLB/DRAM 延迟模型、无频率概念),C4 阶梯、C5、C6-cold、C12 等缓存/TLB 敏感用例的曲线形态与绝对值**均不成立**;
   - 变体相对排序(尤其 pure 在仿真下偏低)存在仿真偏差,设计裁决(C13 结论)以 x86 原生数据为准。
   - **后续:在真实 AArch64 目标机(A76 类 / V 系各一台,test.md §7)上运行同一框架,产出可横向对比的 ARM 原生曲线。**
3. **微架构覆盖**:`i7-3960X` 为单点(Sandy Bridge-E,无 BMI1 → v3 变体仅编译验证);C13 结论迁移性需按 test.md §7 至少补测一代新核;STLB 容量模型(512 项)为本代微架构参数,新核(更大 STLB)上 K=1M 段的 dTLB 归因比例会不同。
4. **环境控制**:本次未做 isolcpus/关 SMT 兄弟/锁频(单用户工作站),已用 ABBA 成对做差 + 30 次重复抑制漂移;c4@64K/1M 档 MAD ~±1 c/op、c11 三档 Δ<1 c/op 的行 MAD/|Δ| 偏大,已在噪声报告标注,正式发布数据建议锁频复测。
5. 未实现变体 pf(表项预取)/ gs(GS-base)维持 N/A 声明(run_all.sh:软件流水破坏逐指令对应 / 0 条转换为理论界,不参与排名)。

---

## 7. 测试产物清单

| 产物 | 说明 |
|---|---|
| `results/run_all_20261125_133125.log` | x86 全量测试日志(C0 gate → 全用例扫描 → C13 → 汇总,**rc=0 全 PASS**) |
| `results/scan_x86_64_20261125_133125.csv` | x86 C1–C14 全用例扫描(31 行,含 PMU ins/l1m/llcm/dtlb 列) |
| `results/c13_x86_64_20261125_133125.csv` | x86 C13 变体矩阵(32 行) |
| `results/c13_status_20261125_133125.txt` | x86 变体状态(9 OK + v3 COMPILE-ONLY + aarch64-cross-compile OK) |
| `results/analyze_20261125_133125.txt` | x86 汇总分析(阶梯/Δins 校验/排名/噪声) |
| `results/run_arm_qemu_20261125_133239.log` | AArch64 交叉编译 + qemu 运行全量日志 |
| `results/scan_aarch64_qemu_20261125_133239.csv` | AArch64 C1–C14 全用例扫描(31 行,cntvct 回退计时) |
| `results/c13_aarch64_qemu_20261125_133239.csv` | AArch64 C13 变体矩阵(24 行) |
| `results/c13_aarch64_status_20261125_133239.txt` | AArch64 变体状态(6 OK) |
| `results/codegen_aarch64_20261125_133239.txt` | AArch64 转换循环 codegen 存档(§8.1 静态核对依据) |
| `results/analyze_dual_20261125_133239.txt` | **双架构合并分析**(含 §4 架构对比表:Δ/IPC/miss) |
| `results/arm_smoke_*.o` | clang AArch64 交叉编译冒烟产物(4 变体) |
| `/tmp/opencode/pmu_probe*.c`(未入库) | PMU 内核兼容性诊断探针(§2.4 三 quirk 的定位证据) |

> 第 1 轮 rdtsc 时代产物(`*_124243.*` / `*_124448.*`)已全部作废,由本轮(`*_133125.*` / `*_133239.*`)取代。

复现命令:

```bash
# x86_64 原生全量(需 perf_event_paranoid ≤ 1)
bench/run_all.sh

# AArch64 交叉编译 + qemu(功能等价验证 + 仿真性能参考)
make ARCH=aarch64 CC=aarch64-linux-gnu-gcc BIN=build/vva_bench_arm
qemu-aarch64 -L /usr/aarch64-linux-gnu ./build/vva_bench_arm --case c0
qemu-aarch64 -L /usr/aarch64-linux-gnu ./build/vva_bench_arm --case c2,c4 --reps 30 --csv out.csv
```
