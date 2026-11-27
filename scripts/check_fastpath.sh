#!/usr/bin/env bash
# check_fastpath.sh — 快路径形态抽查（design.md §5.2/§5.6，test.md §12#4）。
# 反汇编被测二进制，核对 strict 快路径融合序列是否按 §5.2 目标形态生成。
# 用法：scripts/check_fastpath.sh [binary]   （默认 build/vamap_bench）
set -u

BIN=${1:-build/vamap_bench}
[ -x "$BIN" ] || { echo "check_fastpath: $BIN 不存在或不可执行"; exit 2; }
command -v objdump >/dev/null 2>&1 || { echo "check_fastpath: objdump 不可用"; exit 2; }

ARCH=$(uname -m)
DUMP=$(objdump -d "$BIN" 2>/dev/null) || { echo "check_fastpath: 反汇编失败"; exit 2; }

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

echo "check_fastpath: $BIN ($ARCH)"

if [ "$ARCH" = "x86_64" ]; then
    MASK='and +\$0xfffff,%e[a-z0-9]+'
    have "strict 掩码 andl \$0xfffff,%e?? 存在"          "$MASK"
    have "表 load（比例变址 ,%r??,8）存在"                'mov +\(%r[a-z0-9]+,%r[a-z0-9]+,8\),%r[a-z0-9]+'
    have "慢路径 call vamap_slow_create_fill 存在"       'call.*<vamap_slow_create_fill>'

    # 取第一处掩码附近的窗口，核对 §5.2 序列顺序
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

    # 弱校验：call 次数 ≤ 掩码次数（快路径不无条件调用慢路径）
    count_le "call 次数 ≤ 掩码次数" \
        'call.*<vamap_slow_create_fill>' \
        "$MASK"

elif [ "$ARCH" = "aarch64" ]; then
    have "ubfx 提取 idx（vva[51:32]）"   'ubfx +x[0-9]+, x[0-9]+, #32, #20'
    have "ldar acquire 表 load"          '\bldar\b'
    have "cbnz 判零分支"                 '\bcbnz\b'
    have "bfi 合并 offset"               '\bbfi\b'
    have "bl vamap_slow_create_fill"     'bl.*<vamap_slow_create_fill>'
else
    echo "  (跳过：架构 $ARCH 无 §5.2/§5.3 目标序列)"
fi

echo "check_fastpath: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
