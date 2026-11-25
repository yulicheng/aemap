#!/usr/bin/env python3
# bench/analyze.py —— CSV 汇总分析(test.md §5.2)
#
# 产出:
#   1. C4/C5 K-Δ 阶梯表(核心;x86 与 ARM 同表双线,文本版阶梯曲线)
#   2. C2 下界表 + Δins/op 与 design.md §8.1 的一致性校验(自动 PASS/FAIL)
#   3. C13 变体排名表(热 = c2 / 冷 = c4@K=1M 两栏,按编译器分栏)
#   4. 架构对比表(同用例 Δ、IPC、每访存 miss 差异;两架构数据齐备时)
#   5. 噪声报告(MAD/|Δ| > 5% 的配置清单 + rdtsc 回退清单)
#
# 用法:
#   python3 bench/analyze.py scan_xxx.csv [c13_xxx.csv ...] [--status c13_status.txt]
#
# CSV 列(test.md §5.1 + 扩展):arch,variant,enc,tag,base,isa,w,case,param,rep,
#   direct_cyc,conv_cyc,n_ops,direct_ins,conv_ins,direct_l1m,conv_l1m,
#   direct_l2m,conv_l2m,direct_llcm,conv_llcm,direct_dtlb,conv_dtlb,freq_mhz,
#   pmu,mad_cyc_op,ins_expect,ins_ok
import csv
import sys
import os
from collections import defaultdict

# ---------------- design.md §8.1 指令数契约(与实现头 VVA_CONV_INSN 对应) ----------------
# x86: B8 strict 5 / fast 4 / v3(bextr) 4;A4 strict 6 / fast 5 / v3 5
# ARM: strict 4 / fast 3(与编码、BASE 无关;mat/abs 的基址物化循环内摊薄为 0)
# SEG / PURE 为分支型/编译器生成:ins_expect=0,跳过硬校验
def expected_insn(arch, enc, tag, isa):
    if arch == "x86_64":
        if enc == "B8":
            if tag == "fast":
                return 4
            return 4 if isa == "v3" else 5
        else:  # A4
            if isa == "v3":
                return 5
            return 5 if tag == "fast" else 6
    return 3 if tag == "fast" else 4


def fnum(x, nd=2):
    try:
        return f"{float(x):.{nd}f}"
    except (ValueError, TypeError):
        return str(x)


class Row:
    def __init__(self, d):
        self.__dict__.update(d)
        for k in ("direct_cyc", "conv_cyc", "n_ops", "direct_ins", "conv_ins",
                  "direct_l1m", "conv_l1m", "direct_l2m", "conv_l2m",
                  "direct_llcm", "conv_llcm", "direct_dtlb", "conv_dtlb",
                  "freq_mhz", "mad_cyc_op", "ins_expect"):
            try:
                setattr(self, k, float(getattr(self, k, 0) or 0))
            except ValueError:
                setattr(self, k, 0.0)
        self.n_ops = int(self.n_ops)
        try:
            self.pmu = int(float(self.pmu))
        except ValueError:
            self.pmu = 0
        try:
            self.ins_ok = int(float(self.ins_ok))
        except ValueError:
            self.ins_ok = -1
        # K 从 param 解析(c4/c5/c7/c9/c10)
        self.K = 0
        for tok in (self.param or "").split(","):
            if tok.startswith("k="):
                try:
                    self.K = int(tok[2:])
                except ValueError:
                    pass

    @property
    def dop(self):
        """Δcyc/op:配对中位差 ≈ 中位差之差(ABBA 成对),按归一化分母换算"""
        if not self.n_ops:
            return 0.0
        return (self.conv_cyc - self.direct_cyc) / self.n_ops

    @property
    def mad_op(self):
        return self.mad_cyc_op if self.n_ops else 0.0

    @property
    def key(self):
        return (self.arch, self.variant, self.enc, self.tag, self.base,
                self.isa, self.case, self.param)


def load(paths):
    rows, seen = [], set()
    for path in paths:
        if not os.path.exists(path) or os.path.getsize(path) == 0:
            print(f"(跳过空/缺失 CSV:{path})")
            continue
        with open(path) as f:
            for d in csv.DictReader(f):
                d.pop(None, None)       # 多余字段(防御)
                r = Row(d)
                if r.key in seen:       # 跨 CSV 重跑取一条
                    continue
                seen.add(r.key)
                rows.append(r)
    return rows


def hdr(title):
    print("\n" + "=" * 78)
    print(title)
    print("=" * 78)


def table(headers, rows):
    widths = [max(len(str(h)), *(len(str(r[i])) for r in rows)) if rows else len(h)
              for i, h in enumerate(headers)]
    fmt = "  ".join(f"{{:{w}}}" for w in widths)
    print(fmt.format(*[str(h) for h in headers]))
    print("  ".join("-" * w for w in widths))
    for r in rows:
        print(fmt.format(*[str(x) for x in r]))


# ================= 1. C4/C5 K-Δ 阶梯 =================
def ladder(rows):
    hdr("1. C4/C5 K-Δ 阶梯(表足迹穿越 L1/L2/L3/DRAM;test.md §4.5/§4.6)")
    first = {}                      # (arch, case, K) → 首行(扫描 CSV 的默认变体优先)
    order = []
    for r in rows:
        if r.case in ("c4", "c4b", "c5") and r.K:
            k = (r.arch, r.case, r.K)
            if k not in first:
                first[k] = r
                order.append(k)
    if not first:
        print("(无 C4/C5 数据)")
        return
    by_arch = defaultdict(list)
    for k in order:
        by_arch[k[0]].append(first[k])
    for arch, rs in sorted(by_arch.items()):
        rs.sort(key=lambda r: (r.case, r.K))
        variants = sorted({r.variant for r in rs})
        print(f"\n--- {arch}(变体:{', '.join(variants)}) ---")
        t = []
        for r in rs:
            t.append([r.case, f"K={r.K}",
                      f"Δ={r.dop:+.2f}±{r.mad_op:.2f} c/op",
                      f"direct={fnum(r.direct_cyc / max(r.n_ops, 1))}",
                      f"conv={fnum(r.conv_cyc / max(r.n_ops, 1))}",
                      "rdtsc" if not r.pmu else "PMU"])
        table(["case", "K", "Δcyc/op", "direct c/op", "conv c/op", "计时"], t)
        # 阶梯特征提示
        c4 = [r for r in rs if r.case == "c4"]
        if len(c4) >= 3:
            ks = [r.K for r in c4]
            ds = [r.dop for r in c4]
            print(f"  阶梯:K={ks[0]}→{ks[-1]},Δ {ds[0]:+.2f}→{ds[-1]:+.2f} c/op"
                  f"(预期:热段平坦 +1~3,越过缓存容量后跳升,DRAM 段 +100 量级)")


# ================= 2. C2 下界 + Δins 校验 =================
def ins_check(rows):
    hdr("2. C2 下界表 + Δins/op 与 design.md §8.1 一致性校验")
    c2 = [r for r in rows if r.case == "c2"]
    if c2:
        t = [[r.arch, r.variant, f"Δ={r.dop:+.2f}±{r.mad_op:.2f} c/op",
              "PASS(≤3)" if r.dop <= 3 else "超标(待实测校准)"]
             for r in sorted(c2, key=lambda r: (r.arch, r.variant))]
        table(["arch", "variant", "Δcyc/op", "验收(§6:≤3)"], t)
    else:
        print("(无 C2 数据)")

    print("\nΔins/op 硬校验(仅单转换循环内核;±0;需要 PMU):")
    chk = [r for r in rows if r.ins_expect > 0]
    if not chk:
        print("  (无硬校验数据:PMU 不可用或全部为 SEG/PURE 构建)")
        return
    fails = 0
    t = []
    for r in sorted(chk, key=lambda r: (r.arch, r.variant, r.case)):
        exp_doc = expected_insn(r.arch, r.enc, r.tag, r.isa)
        row_ok = r.ins_ok
        build_ok = (int(r.ins_expect) == exp_doc)
        if row_ok == 0:
            verdict = "✗ FAIL(实测≠期望)"
            fails += 1
        elif row_ok == 1:
            verdict = "✓ PASS"
        elif not build_ok:
            verdict = f"✗ 构建期望 {int(r.ins_expect)} ≠ §8.1 文档 {exp_doc}"
            fails += 1
        else:
            verdict = "n/a(非单转换循环内核,不参与硬校验;codegen 静态核对)"
        t.append([r.arch, r.variant, r.case, f"{int(r.ins_expect)}",
                  f"{exp_doc}", verdict])
    table(["arch", "variant", "case", "构建期望", "§8.1 文档", "校验"], t)
    if fails:
        print(f"\n  ⚠ {fails} 条硬校验失败!")


# ================= 3. C13 变体排名 =================
def ranking(rows):
    hdr("3. C13 变体排名(热 = c2;冷 = c4@K=1M;按编译器分栏)")
    hot = {}
    cold = {}
    for r in rows:
        if r.case == "c2":
            hot[(r.arch, r.variant)] = r
        if r.case == "c4" and r.K == 1048576:
            cold[(r.arch, r.variant)] = r
    if not hot and not cold:
        print("(无 C13 数据)")
        return
    for arch in sorted({a for a, _ in list(hot) + list(cold)}):
        print(f"\n--- {arch} ---")
        variants = sorted({v for a, v in hot if a == arch} |
                          {v for a, v in cold if a == arch})
        t = []
        for v in variants:
            h = hot.get((arch, v))
            c = cold.get((arch, v))
            # 编译器分栏:variant 名前缀(gcc/clang)即分栏键
            t.append([v.split("-")[0], v,
                      f"{h.dop:+.2f}±{h.mad_op:.2f}" if h else "-",
                      f"{c.dop:+.2f}±{c.mad_op:.2f}" if c else "-"])
        table(["cc", "variant", "热 Δcyc/op(c2)", "冷 Δcyc/op(c4@1M)"], t)
        hv = sorted(((hot[(arch, v)].dop, v) for v in variants if (arch, v) in hot))
        if hv:
            print(f"  热排名:{' > '.join(v for _, v in hv)}(Δ 越小越好)")
        cv = sorted(((cold[(arch, v)].dop, v) for v in variants if (arch, v) in cold))
        if cv:
            print(f"  冷排名:{' > '.join(v for _, v in cv)}")


# ================= 4. 架构对比 =================
def arch_cmp(rows):
    hdr("4. 架构对比(同用例 Δ、IPC、每访存 miss;两架构齐备时输出)")
    by_case = defaultdict(dict)
    for r in rows:
        by_case[(r.case, r.param, r.variant)][r.arch] = r
    if not any(len(v) > 1 for v in by_case.values()):
        print("(仅单架构数据,跳过)")
        return
    t = []
    for (case, param, variant), d in sorted(by_case.items()):
        if len(d) < 2:
            continue
        line = [f"{case} {param}", variant]
        for arch in ("x86_64", "aarch64"):
            r = d.get(arch)
            if not r:
                line.append("-")
                line.append("-")
                line.append("-")
                continue
            ipc = (r.direct_ins / r.direct_cyc) if r.pmu and r.direct_cyc else 0
            line.append(f"{r.dop:+.2f}")
            line.append(f"IPC {ipc:.2f}" if ipc else "IPC n/a")
            line.append(f"l1m/op {abs(r.conv_l1m - r.direct_l1m) / max(r.n_ops, 1):.1e}")
        t.append(line)
    table(["case", "variant",
           "x86 Δc/op", "x86 IPC", "x86 Δl1m/op",
           "arm Δc/op", "arm IPC", "arm Δl1m/op"], t)


# ================= 5. 噪声报告 =================
def noise(rows):
    hdr("5. 噪声报告(MAD/|Δ| > 5%;test.md §6:90% 配置 MAD/median ≤ 5%)")
    bad, rdtsc = [], []
    for r in rows:
        if not r.pmu:
            rdtsc.append(r)
        d = abs(r.dop)
        if d > 0.01 and r.mad_op > 0.05 * d:
            bad.append(r)
        elif d <= 0.01 and r.mad_op > 0.05 * max(d, 0.05):
            bad.append(r)   # Δ≈0 但绝对 MAD 不可忽略
    if rdtsc:
        print(f"\nrdtsc/cntvct 回退计时行(无 PMU,Δ 仅墙钟近似):{len(rdtsc)} 条")
        for r in rdtsc[:8]:
            print(f"    {r.arch} {r.variant} {r.case} {r.param}")
        if len(rdtsc) > 8:
            print(f"    ... 共 {len(rdtsc)} 条")
    if bad:
        print(f"\n不稳定配置(MAD/|Δ| > 5%):{len(bad)} 条")
        for r in sorted(bad, key=lambda r: -r.mad_op)[:12]:
            print(f"    {r.arch} {r.variant} {r.case} {r.param}: "
                  f"Δ={r.dop:+.2f}±{r.mad_op:.2f} c/op")
    else:
        print("全部配置稳定(MAD/|Δ| ≤ 5%)✓")

    # C8 带宽(由 freq 列换算,§5.2-4 的补充)
    c8 = [r for r in rows if r.case == "c8" and r.freq_mhz > 0]
    if c8:
        print("\nC8 带宽参考(freq 换算):")
        t = []
        for r in c8:
            gbd = r.n_ops * r.freq_mhz * 1e6 / max(r.direct_cyc, 1) / 1e9
            gbc = r.n_ops * r.freq_mhz * 1e6 / max(r.conv_cyc, 1) / 1e9
            t.append([r.variant, r.param, f"{gbd:.2f} GB/s", f"{gbc:.2f} GB/s",
                      f"{(gbc / gbd - 1) * 100:+.1f}%" if gbd else "-"])
        table(["variant", "param", "direct", "conv", "Δ%"], t)


def main():
    argv = sys.argv[1:]
    status_path = None
    if "--status" in argv:
        i = argv.index("--status")
        status_path = argv[i + 1]
        del argv[i:i + 2]
    args = argv
    if not args:
        print(__doc__)
        sys.exit(1)
    rows = load(args)
    if not rows:
        print("无有效数据行")
        sys.exit(1)
    print(f"载入 {len(rows)} 行(来自 {len(args)} 个 CSV)")
    if status_path and os.path.exists(status_path):
        print(f"C13 变体状态({status_path}):")
        for line in open(status_path):
            line = line.strip()
            if line:
                print(f"    {line}")
        print("    N/A:pf(软件流水破坏逐指令对应)/ gs(0 条转换,理论界,不参与排名)")
    ladder(rows)
    ins_check(rows)
    ranking(rows)
    arch_cmp(rows)
    noise(rows)


if __name__ == "__main__":
    main()
