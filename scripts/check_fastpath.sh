#!/usr/bin/env bash
# check_fastpath.sh — 快路径形态抽查（design.md §5.2/§5.6，test.md §12#4）。
# 反汇编被测二进制，核对快路径融合序列是否按 §5.2/§5.3 目标形态生成。
# 用法：scripts/check_fastpath.sh [binary] [variant]
#   variant ∈ strict | fast-tag0 | bmi2 | sve2-batch
#   缺省时先读二进制内嵌 build 标签（bench/main.c 中的 tag），读不出再按
#   指令特征兜底（shrx→bmi2、ld1d 聚集→sve2-batch、否则按 strict）。
#   fast-tag0 构建无法按特征自动识别（C 标量路径仍含 $0xfffff 掩码），
#   建议始终显式传变体（make check-fastpath 已传 $(VARIANT)）。
set -u

BIN=${1:-build/vamap_bench}
VARIANT=${2:-}
[ -x "$BIN" ] || { echo "check_fastpath: $BIN 不存在或不可执行"; exit 2; }
command -v objdump >/dev/null 2>&1 || { echo "check_fastpath: objdump 不可用"; exit 2; }

# 反汇编：native objdump 失败时尝试 aarch64 交叉 objdump（跨架构产物）
DUMP=$(objdump -d "$BIN" 2>/dev/null) || DUMP=
if [ -z "$DUMP" ] && command -v aarch64-linux-gnu-objdump >/dev/null 2>&1; then
    DUMP=$(aarch64-linux-gnu-objdump -d "$BIN" 2>/dev/null) || DUMP=
fi
[ -n "$DUMP" ] || { echo "check_fastpath: 反汇编失败"; exit 2; }

# 被测二进制的目标架构（file 识别，失败回退主机架构）
BARCH=$(uname -m)
FTYPE=$(file -b "$BIN" 2>/dev/null || true)
case "$FTYPE" in
    *aarch64*) BARCH=aarch64 ;;
    *x86-64*)  BARCH=x86_64 ;;
esac

PASS=0
FAIL=0

# 在指定文本中查找扩展正则
have_in() { # $1=文本 $2=描述 $3=grep 扩展模式
    if printf '%s\n' "$1" | grep -qE "$3"; then
        echo "  [ok]   $2"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $2"
        FAIL=$((FAIL + 1))
    fi
}

have() { # 全量反汇编查找
    have_in "$DUMP" "$1" "$2"
}

have_not_in() { # 反向断言：窗口内不得出现
    if printf '%s\n' "$1" | grep -qE "$3"; then
        echo "  [FAIL] $2"
        FAIL=$((FAIL + 1))
    else
        echo "  [ok]   $2"
        PASS=$((PASS + 1))
    fi
}

count_le() { # $1=描述 $2=模式A(少) $3=模式B(多)
    NA=$(printf '%s\n' "$DUMP" | grep -cE "$2" || true)
    NB=$(printf '%s\n' "$DUMP" | grep -cE "$3" || true)
    if [ "${NA:-0}" -le "${NB:-0}" ]; then
        echo "  [ok]   $1 ($NA <= $NB)"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $1 ($NA > $NB)"
        FAIL=$((FAIL + 1))
    fi
}

# 快路径窗口：取首个「比例变址8 load + 2 行内 test + 3 行内 jne」三联锚
fast_window() {
    local LN
    LN=$(printf '%s\n' "$DUMP" | awk '
        /mov +\(%r[a-z0-9]+,%r[a-z0-9]+,8\),%r[a-z0-9]+/ { ln = NR; l = 1; next }
        l && NR - ln <= 2 && /test +%r[a-z0-9]+,%r[a-z0-9]+/ { t = 1; next }
        l && t && NR - ln <= 3 && /jne +[0-9a-f]+/ { print ln; exit }
        { l = 0; t = 0 }')
    [ -n "${LN:-}" ] || return 1
    printf '%s\n' "$DUMP" | sed -n "$((LN - 7)),$((LN + 20))p"
}

echo "check_fastpath: $BIN ($BARCH)"

# ---- 变体判定：显式参数 > 内嵌 build 标签 > 指令特征 ----
IS_BMI2=0; IS_SVE2=0; IS_FAST=0
if [ -z "$VARIANT" ] && command -v strings >/dev/null 2>&1; then
    VARIANT=$(strings "$BIN" 2>/dev/null \
        | grep -xE '(strict|fast-tag0|bmi2|sve2-batch)(-idx[0-9]+)?' | head -1)
fi
case "$VARIANT" in
    bmi2)        IS_BMI2=1 ;;
    fast-tag0*)  IS_FAST=1 ;;
    sve2-batch*) IS_SVE2=1 ;;
    strict*)     ;;  # 显式 strict
    *)
        printf '%s\n' "$DUMP" | grep -qE 'shrx +%r' && IS_BMI2=1
        printf '%s\n' "$DUMP" | grep -qE 'ld1d[[:space:]]+\{z[0-9]+\.d\}, p[0-9]+/z, \[x[0-9]+, z[0-9]+\.d, lsl #3\]' && IS_SVE2=1
        ;;
esac
if   [ "$IS_BMI2" = 1 ]; then echo "  (变体 bmi2)"
elif [ "$IS_SVE2" = 1 ]; then echo "  (变体 sve2-batch)"
elif [ "$IS_FAST" = 1 ]; then echo "  (变体 fast-tag0)"
else echo "  (变体 strict)"
fi

# 三联锚窗口（仅 x86 快路径形态检查用）
FW=""
if [ "$BARCH" = "x86_64" ] && ! FW=$(fast_window); then FW=""
fi

if [ "$BARCH" = "x86_64" ]; then
    MASK='and +\$0xfffff,%e[a-z0-9]+'

    if [ "$IS_FAST" = 1 ]; then
        # §5.2 fast（tag≡0 契约）：无 andl 掩码。基线 fast 7（mov+shr 提取）；
        # 若该构建同时带 -mbmi2（fast 6 条口径）则 shrx 取代 mov+shr。
        if [ -n "$FW" ]; then
            have_in "$FW" "fast 窗口：vva 复制 mov → %r??"       'mov +%r[a-z0-9]+,%r[a-z0-9]+'
            have_in "$FW" "fast 窗口：shr \$0x20 提取 vva[63:32]" 'shr +\$0x20,%r[a-z0-9]+'
            have_not_in "$FW" "fast 窗口：无 andl \$0xfffff（tag≡0 契约）" "$MASK"
            NLOADS=$(printf '%s\n' "$FW" | grep -cE 'mov +\(%r[a-z0-9]+,%r[a-z0-9]+,8\),%r[a-z0-9]+' || true)
            if [ "${NLOADS:-0}" = "1" ]; then
                echo "  [ok]   fast 窗口：唯一一次表 load（窗口内 1 条）"
                PASS=$((PASS + 1))
            else
                echo "  [FAIL] fast 窗口：唯一一次表 load（窗口内 ${NLOADS:-0} 条）"
                FAIL=$((FAIL + 1))
            fi
            have_in "$FW" "fast 窗口：test 判零"                 'test +%r[a-z0-9]+,%r[a-z0-9]+'
            have_in "$FW" "fast 窗口：条件分支（jne 跳过慢路径）" 'jne +[0-9a-f]+'
            have_in "$FW" "fast 窗口：慢路径 call"               'call.*<vamap_slow_create_fill>'
            have_in "$FW" "fast 窗口：零扩展 32 位 mov → %e?x"    'mov +(%e[a-z]+|%r[a-z0-9]+d),%[a-z]+'
            have_in "$FW" "fast 窗口：合并 or %rcx,%r??"          'or +%rcx,%r[a-z0-9]+'
        else
            echo "  [FAIL] fast：未找到「load+test+jne」快路径窗口"
            FAIL=$((FAIL + 1))
        fi
        # 弱校验：慢路径 call 不多于判零 test（fast 无掩码指令作对照）
        count_le "call 次数 ≤ test 判零次数" \
            'call.*<vamap_slow_create_fill>' \
            'test +%r[a-z0-9]+,%r[a-z0-9]+'
    else
        have "strict 掩码 andl \$0xfffff,%e?? 存在"          "$MASK"
        have "表 load（比例变址 ,%r??,8）存在"                'mov +\(%r[a-z0-9]+,%r[a-z0-9]+,8\),%r[a-z0-9]+'
        have "慢路径 call vamap_slow_create_fill 存在"       'call.*<vamap_slow_create_fill>'

        if [ "$IS_BMI2" = 1 ]; then
            # §5.2 BMI2 变体（strict 7）：shrx 取代 mov+shr，循环外预置
            # 移位量寄存器，vva 原样保留（无快路径复制）。
            # 锚点用 and $0xfffff 掩码（-mbmi2 下编译器也会自生成 shrx，
            # 不能以首个 shrx 为锚）
            LN=$(printf '%s\n' "$DUMP" | grep -nE "$MASK" | head -1 | cut -d: -f1)
            if [ -n "${LN:-}" ] && [ "${LN:-0}" -gt 0 ] 2>/dev/null; then
                SEQ=$(printf '%s\n' "$DUMP" | sed -n "$((LN - 3)),$((LN + 12))p")
                have_in "$SEQ" "bmi2 序列：shrx 提取 vva[63:32]"   'shrx +%r[a-z0-9]+,%r[a-z0-9]+,%r[a-z0-9]+'
                have_in "$SEQ" "bmi2 序列：andl \$0xfffff 掩 idx"  "$MASK"
                have_in "$SEQ" "bmi2 序列：test 判零"              'test +%r[a-z0-9]+,%r[a-z0-9]+'
                have_in "$SEQ" "bmi2 序列：条件分支（jne 慢路径）" 'jne +[0-9a-f]+'
                have_in "$SEQ" "bmi2 序列：零扩展 32 位 mov → %e?x" 'mov +(%e[a-z]+|%r[a-z0-9]+d),%[a-z]+'
                have_in "$SEQ" "bmi2 序列：合并 or"                'or +%r[a-z0-9]+,%r[a-z0-9]+'
                if printf '%s\n' "$SEQ" | grep -qE 'shr +\$0x20'; then
                    echo "  [FAIL] bmi2 序列：不应出现 mov+shr（应由 shrx 取代）"
                    FAIL=$((FAIL + 1))
                else
                    echo "  [ok]   bmi2 序列：无 mov+shr 复制（shrx 保留 vva）"
                    PASS=$((PASS + 1))
                fi
            else
                echo "  [FAIL] bmi2：未找到掩码序列窗口"
                FAIL=$((FAIL + 1))
            fi
        else
            # §5.2 strict 基线（strict 8）
            LN=$(printf '%s\n' "$DUMP" | grep -nE "$MASK" | head -1 | cut -d: -f1)
            if [ -n "${LN:-}" ] && [ "${LN:-0}" -gt 0 ] 2>/dev/null; then
                SEQ=$(printf '%s\n' "$DUMP" | sed -n "$((LN - 3)),$((LN + 14))p")
                have_in "$SEQ" "序列：vva 复制 mov → %r??"          'mov +%r[a-z0-9]+,%r[a-z0-9]+'
                have_in "$SEQ" "序列：shrq \$32 提取 vva[63:32]"    'shr +\$0x20,%r[a-z0-9]+'
                have_in "$SEQ" "序列：andl \$0xfffff 掩 idx"        "$MASK"
                NLOADS=$(printf '%s\n' "$SEQ" | grep -cE 'mov +\(%r[a-z0-9]+,%r[a-z0-9]+,8\),%r[a-z0-9]+' || true)
                if [ "${NLOADS:-0}" = "1" ]; then
                    echo "  [ok]   序列：唯一一次表 load（窗口内 1 条）"
                    PASS=$((PASS + 1))
                else
                    echo "  [FAIL] 序列：唯一一次表 load（窗口内 ${NLOADS:-0} 条）"
                    FAIL=$((FAIL + 1))
                fi
                have_in "$SEQ" "序列：test 判零"                    'test +%r[a-z0-9]+,%r[a-z0-9]+'
                have_in "$SEQ" "序列：条件分支（jne/je 慢路径）"    '(jne|je) +[0-9a-f]+'
                have_in "$SEQ" "序列：零扩展 32 位 mov → %e?x"      'mov +(%e[a-z]+|%r[a-z0-9]+d),%[a-z]+'
                have_in "$SEQ" "序列：合并 or %rcx,%r??"            'or +%rcx,%r[a-z0-9]+'
            else
                echo "  [FAIL] 未找到 strict 掩码指令"
                FAIL=$((FAIL + 1))
            fi
        fi

        # 弱校验：call 次数 ≤ 掩码次数（快路径不无条件调用慢路径）
        count_le "call 次数 ≤ 掩码次数" \
            'call.*<vamap_slow_create_fill>' \
            "$MASK"
    fi

elif [ "$BARCH" = "aarch64" ]; then
    # 标量形态（T1/T8 与正确性路径；sve2-batch 构建同样保留）
    have "ubfx 提取 idx（vva[51:32]）"   'ubfx[[:space:]]+x[0-9]+, x[0-9]+, #32, #20'
    have "ldar acquire 表 load"          '\bldar\b'
    have "cbnz 判零分支"                 '\bcbnz\b'
    have "bfi 合并 offset（bfi/bfxil 别名）"  '(bfi|bfxil)[[:space:]]+x[0-9]+, x[0-9]+'
    have "bl vamap_slow_create_fill"     'bl.*<vamap_slow_create_fill>'

    if [ "$IS_SVE2" = 1 ]; then
        # §5.3 批量形态：rdvl 探测 + 表项 gather + 判零谓词 + b.any 退化分支
        have "rdvl 探测 VL"                  '\brdvl\b'
        have "批量：ld1d 表项 gather（[x??, z??.d, lsl #3]）" \
            'ld1d[[:space:]]+\{z[0-9]+\.d\}, p[0-9]+/z, \[x[0-9]+, z[0-9]+\.d, lsl #3\]'
        have "批量：cmpeq 判零谓词"          'cmpeq[[:space:]]+p[0-9]+\.d, p[0-9]+/z, z[0-9]+\.d, #0'
        have "批量：b.any 整批退化分支"      '\bb\.any\b'
        have "批量：whilelo 批谓词"          '\bwhilelo[[:space:]]+p[0-9]+\.d, xzr'
        have "批量：向量基址数据 gather（[z??.d]）" \
            'ld1d[[:space:]]+\{z[0-9]+\.d\}, p[0-9]+/z, \[z[0-9]+\.d\]'
        have "批量：向量基址 scatter（st1d [z??.d]）" \
            'st1d[[:space:]]+\{z[0-9]+\.d\}, p[0-9]+, \[z[0-9]+\.d\]'
        # §5.3 转换核心的合并掩码（offset-only，两变体一致）
        have "批量：offset 掩码 and #0xffffffff" \
            'and[[:space:]]+z[0-9]+\.d, z[0-9]+\.d, #0xffffffff'
        have "批量：合并 orr z??, z??, z??" \
            'orr[[:space:]]+z[0-9]+\.d, z[0-9]+\.d, z[0-9]+\.d'
        if [ "$IS_FAST" = 1 ]; then
            have_not_in "$DUMP" "批量：fast 无 idx 掩码 and #0xfffff（tag≡0 契约）" \
                'and[[:space:]]+z[0-9]+\.d, z[0-9]+\.d, #0xfffff'
        else
            have "批量：idx 掩码 and #0xfffff" \
                'and[[:space:]]+z[0-9]+\.d, z[0-9]+\.d, #0xfffff'
        fi
    fi
else
    echo "  (跳过：架构 $BARCH 无 §5.2/§5.3 目标序列)"
fi

echo "check_fastpath: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
