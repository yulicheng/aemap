#!/usr/bin/env bash
# bench/run_all.sh —— 全量测试入口(test.md §8 里程碑 M1–M4)
#
# 流程:
#   0. 环境检查与记录(架构 / 编译器 / perf 权限 / clflush / THP / CPU 型号)
#   1. C0 差分正确性 gate(默认变体;失败即中止 —— §4.1 前置 gate)
#   2. C1–C12、C14 全用例扫描(默认变体,含子模式与 C4 K 阶梯)→ results/scan_*.csv
#   3. C13 变体矩阵:每个变体重跑 c0 + c13 别名(c2 + c4@{4K,64K,1M})
#        default / fast / enc 互换 / mat / abs(x86) / pure / seg / clang / v3*
#        * v3 需 CPU 支持 BMI1,否则仅编译并记录 N/A
#        AArch64:本机为 x86 时做 clang 交叉编译冒烟(bench/arm_smoke.c,仅静态验证)
#      C13 结论按编译器分栏(test.md §4.14:x86-64 clang 无 pinned,自动 mat)
#   4. bench/analyze.py 汇总产出(§5.2)→ results/analyze_*.txt
#
# pf(表项预取)与 gs(GS-base)变体未实现,本脚本显式记录 N/A 及理由。
#
# 用法:
#   bench/run_all.sh            # 全量(reps=30)
#   bench/run_all.sh --quick    # 冒烟(reps=8,用于流水线自检)
set -u
cd "$(dirname "$0")/.."

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1
REPS=30; [ "$QUICK" = 1 ] && REPS=8

TS=$(date +%Y%m%d_%H%M%S)
RES=results
mkdir -p "$RES"
LOG="$RES/run_all_$TS.log"

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
note() { echo "$*" | tee -a "$LOG"; }

# ================= 0. 环境检查 =================
ARCH=$(uname -m)
case "$ARCH" in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    *) log "不支持的架构:$ARCH"; exit 1 ;;
esac

log "== 0. 环境检查 =="
log "arch            : $ARCH"
log "kernel          : $(uname -r)"
log "cpu             : $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ //' || true)"
if [ "$ARCH" = aarch64 ]; then
    log "cpu             : $(grep -m1 -E 'CPU implementer|model name' /proc/cpuinfo 2>/dev/null | head -1 || true)"
fi
GCC_V=$(gcc --version 2>/dev/null | head -1 || echo "gcc 不可用")
CLANG_V=$(clang --version 2>/dev/null | head -1 || echo "clang 不可用")
log "gcc             : $GCC_V"
log "clang           : $CLANG_V"

PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "无")
log "perf_paranoid   : $PARANOID"
if [ "$PARANOID" = "无" ] || [ "$PARANOID" -gt 2 ] 2>/dev/null; then
    log "⚠ perf 不可用(paranoid>2 或缺失):PMU 指标与 Δins/op 硬校验将回退标记 n/a,"
    log "  计时回退 lfence;rdtsc / cntvct_el0(§3.1);指令数契约改由 'make codegen' 静态核对。"
    HAVE_PERF=0
else
    HAVE_PERF=1
fi

if [ "$ARCH" = x86_64 ]; then
    log "clflush         : $(grep -q clflush /proc/cpuinfo && echo 支持 || echo 不支持)"
    log "bmi1/bmi2       : $(grep -qm1 bmi1 /proc/cpuinfo && echo 有 || echo 无) / $(grep -qm1 bmi2 /proc/cpuinfo && echo 有 || echo 无)"
fi
THP=$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo "未知")
log "THP             : $THP"

# pf / gs 变体:N/A(不实现)—— 显式记录(test.md §4.14 / design.md §6)
note ""
note "N/A 变体声明(C13 矩阵):"
note "  - pf 表项预取  : N/A。地址序列已预生成,但需将内核改写为软件流水形式,"
note "                    访存次序改变会破坏与 direct 实例的逐指令对应(Δins 校验失效),"
note "                    收益场景(顺序/跨段表 miss)已被 C7 的硬件预取部分覆盖。"
note "  - gs GS-base   : N/A。需 FSGSBASE + 内核 ≥5.9 且 wrgsbase 切换代价(~20-40c)"
note "                    只在段粘性吞吐场景摊薄;与本框架'每访问转换'度量形态不同,"
note "                    0 条转换指令使其成为 C13 表外的理论上界,不参与排名。"

# ================= 1. C0 gate =================
log ""
log "== 1. C0 差分正确性 gate(默认变体)=="
make BIN=build/vva_bench >>"$LOG" 2>&1 || { log "默认变体构建失败"; exit 1; }
if ./build/vva_bench --case c0 >>"$LOG" 2>&1; then
    log "C0 PASS"
else
    log "C0 FAIL —— 性能测试中止(§4.1)"
    tail -5 "$LOG"
    exit 1
fi

# ================= 2. 全用例扫描(默认变体) =================
log ""
log "== 2. C1–C12、C14 全用例扫描(reps=$REPS)=="
SCAN="$RES/scan_${ARCH}_$TS.csv"
run_cases() {  # $1 = --sub 参数(空串表示无),$2 = case 列表
    local sub="$1" cases="$2"
    if [ -n "$sub" ]; then
        ./build/vva_bench --case "$cases" --sub "$sub" --reps "$REPS" --csv "$SCAN" >>"$LOG" 2>&1
    else
        ./build/vva_bench --case "$cases" --reps "$REPS" --csv "$SCAN" >>"$LOG" 2>&1
    fi
    local rc=$?
    [ $rc -ne 0 ] && log "⚠ 用例 [$cases] 退出码 $rc(Δins FAIL 或运行错误,详见日志)"
    return $rc
}
RC_ALL=0
run_cases ""    "c1"            || RC_ALL=1
run_cases "w"   "c1"            || RC_ALL=1
run_cases ""    "c2,c3,c4,c4b"  || RC_ALL=1
run_cases ""    "c5"            || RC_ALL=1
run_cases ""    "c6"            || RC_ALL=1
run_cases "cold" "c6"           || RC_ALL=1
run_cases ""    "c7"            || RC_ALL=1
run_cases ""    "c8"            || RC_ALL=1
run_cases "word" "c8"           || RC_ALL=1
run_cases "nt"   "c8"           || RC_ALL=1
run_cases ""    "c9,c10"        || RC_ALL=1
run_cases ""    "c11"           || RC_ALL=1
run_cases "rmw" "c11"           || RC_ALL=1
run_cases "w"   "c11"           || RC_ALL=1
run_cases ""    "c12"           || RC_ALL=1
run_cases "2m"  "c12"           || RC_ALL=1
run_cases ""    "c14"           || RC_ALL=1
run_cases "matmul" "c14"        || RC_ALL=1

# C4 K 阶梯(test.md §4.5 全扫描:64B→8MB 穿越 L1/L2/L3/DRAM)
log "-- C4 K 阶梯扫描 --"
for K in 8 64 256 1024 4096 16384 65536 262144 1048576; do
    ./build/vva_bench --case c4 --k "$K" --reps "$REPS" --csv "$SCAN" >>"$LOG" 2>&1 || RC_ALL=1
done
log "扫描 CSV:$SCAN"

# ================= 3. C13 变体矩阵 =================
log ""
log "== 3. C13 变体矩阵(每变体:c0 gate + c13 别名 = c2 + c4@{4K,64K,1M})=="
C13="$RES/c13_${ARCH}_$TS.csv"

build_and_run() {  # $1 = 显示名,$2 = make 变量串,$3 = BIN 路径
    local name="$1" margs="$2" bin="$3"
    log "-- 变体 [$name]:make BIN=$bin $margs --"
    if ! eval make "BIN=$bin" $margs "VARIANT=$name" >>"$LOG" 2>&1; then
        log "   构建失败,跳过"
        echo "BUILD-FAIL $name" >> "$RES/c13_status_$TS.txt"
        return 1
    fi
    if ! ./"$bin" --case c0 >>"$LOG" 2>&1; then
        log "   C0 FAIL,跳过该变体的测量"
        echo "C0-FAIL $name" >> "$RES/c13_status_$TS.txt"
        return 1
    fi
    if ! ./"$bin" --case c13 --reps "$REPS" --csv "$C13" >>"$LOG" 2>&1; then
        log "   ⚠ c13 测量返回非零(可能 Δins FAIL,数据已入 CSV)"
        return 1
    fi
    log "   完成"
    echo "OK $name" >> "$RES/c13_status_$TS.txt"
}

: > "$RES/c13_status_$TS.txt"
ENC_SWAP=$([ "$ARCH" = x86_64 ] && echo A4 || echo B8)

build_and_run "default"  ""                "build/vva_bench_m_default"
build_and_run "fast"     "TAG=fast"        "build/vva_bench_m_fast"
build_and_run "enc-swap" "ENC=$ENC_SWAP"   "build/vva_bench_m_enc"
build_and_run "mat"      "BASE=mat"        "build/vva_bench_m_mat"
if [ "$ARCH" = x86_64 ]; then
    build_and_run "abs"  "BASE=abs"        "build/vva_bench_m_abs"
fi
build_and_run "pure"     "PURE=1"          "build/vva_bench_m_pure"
build_and_run "seg"      "SEG=1"           "build/vva_bench_m_seg"
if echo "$CLANG_V" | grep -q clang; then
    build_and_run "clang" "CC=clang"       "build/vva_bench_m_clang"
else
    log "-- 变体 [clang]:N/A(本机无 clang)"
    echo "N/A clang(无编译器)" >> "$RES/c13_status_$TS.txt"
fi

if [ "$ARCH" = x86_64 ] && grep -qm1 bmi1 /proc/cpuinfo; then
    build_and_run "v3"   "ISA=v3"          "build/vva_bench_m_v3"
elif [ "$ARCH" = x86_64 ]; then
    log "-- 变体 [v3]:仅编译验证(CPU 无 BMI1,运行会 SIGILL,附录 A) --"
    if eval make BIN=build/vva_bench_m_v3 ISA=v3 "VARIANT=v3" >>"$LOG" 2>&1; then
        log "   v3 编译通过(codegen 请用 make BIN=build/vva_bench_m_v3 codegen 核对 4 条序列)"
        echo "COMPILE-ONLY v3(CPU 无 BMI1)" >> "$RES/c13_status_$TS.txt"
    else
        echo "BUILD-FAIL v3" >> "$RES/c13_status_$TS.txt"
    fi
fi

# ---- AArch64 交叉编译冒烟(x86 主机上仅静态验证;运行时正确性由目标机 C0 把关) ----
if [ "$ARCH" != aarch64 ] && echo "$CLANG_V" | grep -q clang; then
    log "-- AArch64 交叉编译冒烟(clang --target=aarch64-linux-gnu -ffreestanding) --"
    ARM_OK=1
    for spec in "strict-pinned:-DVVA_ENC_A4 -DVVA_BASE_PINNED -ffixed-x27" \
                "fast-pinned:-DVVA_ENC_A4 -DVVA_BASE_PINNED -DVVA_TAG_FAST -ffixed-x27" \
                "enc-swap-mat:-DVVA_ENC_B8" \
                "strict-seg:-DVVA_ENC_A4 -DVVA_SEG_CACHE"; do
        name="${spec%%:*}"; flags="${spec#*:}"
        # shellcheck disable=SC2086
        if clang --target=aarch64-linux-gnu -ffreestanding -O2 -std=c11 -Wall -Wextra \
                 -I. -Iinclude -Iarch/aarch64 $flags -c bench/arm_smoke.c \
                 -o "$RES/arm_smoke_${name}.o" >>"$LOG" 2>&1; then
            log "   AArch64 [$name] 编译通过(含 §8.1 指令数 _Static_assert)"
        else
            log "   AArch64 [$name] 编译失败"
            ARM_OK=0
        fi
    done
    [ $ARM_OK = 1 ] && echo "OK aarch64-cross-compile" >> "$RES/c13_status_$TS.txt" \
                    || echo "FAIL aarch64-cross-compile" >> "$RES/c13_status_$TS.txt"
fi
cat "$RES/c13_status_$TS.txt" | while read -r st name rem; do
    log "变体状态:$st $name ${rem:-}"
done

# ================= 4. 汇总分析 =================
log ""
log "== 4. analyze.py 汇总(test.md §5.2)=="
REPORT="$RES/analyze_$TS.txt"
if command -v python3 >/dev/null 2>&1; then
    python3 bench/analyze.py "$SCAN" "$C13" --status "$RES/c13_status_$TS.txt" \
        2>&1 | tee "$REPORT" | tail -40
    log "报告:$REPORT"
else
    log "python3 不可用,跳过汇总"
fi

log ""
log "== 完成:rc_all=$RC_ALL 产物:$SCAN / $C13 / $REPORT =="
exit $RC_ALL
