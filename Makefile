# Makefile —— vva_bench 构建矩阵(design.md §5.4 / test.md §2.1)
#
# 变量:
#   ARCH  x86_64 / aarch64     默认 uname -m(仅原生构建;AArch64 交叉验证用
#                              bench/arm_smoke.c,见 bench/run_all.sh)
#   ENC   B8 / A4              默认 auto(x86_64→B8,aarch64→A4;C13 可互换)
#   TAG   strict / fast        默认 strict(fast = tag≡0 契约,省 1 条)
#   BASE  pinned / mat / abs   默认 pinned(abs 仅 x86;x86 clang 不支持
#                              -ffixed-r15 → 自动回退 mat 并告警,§7)
#   ISA   v1 / v3              默认 v1(v3 = x86-64-v3 BMI1 bextr,仅 x86)
#   PURE  1                    纯 C 对照变体(design §6)
#   SEG   1                    段缓存变体(design §6 `cache`)
#   HP    1                    表 2MB 对齐,可 madvise hugepage(test.md C12)
#   W     VVA_HIGH_BITS        默认 16
#   WMAX  VVA_W_MAX 覆盖       默认按 vva_common.h(x86 15 / arm 16)
#   CC    gcc / clang          默认 gcc
#   BIN   输出路径             默认 build/vva_bench;OBJ 独立目录(= build/obj/$(notdir BIN)),
#                              多变体并行构建互不覆盖
#   VARIANT CSV variant 列     默认自动派生(<cc>-<tag>-<base>[-<enc>][-v3|-pure|-seg|-hp])
#
# 目标:
#   all(默认)  构建 $(BIN)
#   check        构建 + 运行 C0 差分正确性 gate(test.md §4.1)
#   codegen      objdump 抽取 k_read_c / k_read_d 转换循环(编译器存档,§7)
#   clean
#
# ⚠ -fno-tree-vectorize -fno-slp-vectorize 是 Δins/op 硬校验(design §8.1)的前提:
#   向量化会把 direct 实例的访存循环改成 SIMD,而 conv 实例因 volatile asm
#   保持标量,kernel_defs.h 的"逐指令对应"被破坏,Δins 失去意义。

CC      ?= gcc
ARCH    ?= $(shell uname -m)
ifeq ($(ARCH),amd64)
  ARCH := x86_64
endif
ifeq ($(ARCH),arm64)
  ARCH := aarch64
endif
ifneq ($(ARCH),x86_64)
ifneq ($(ARCH),aarch64)
  $(error ARCH 仅支持 x86_64 / aarch64(得到:$(ARCH)))
endif
endif

CLANG   := $(findstring clang,$(CC))
CC_ID   := $(if $(CLANG),clang,gcc)

# ---- ENC 解析 ----
ifeq ($(ARCH),x86_64)
  ENC_DEF := B8
else
  ENC_DEF := A4
endif
ENC ?= auto
ifeq ($(ENC),auto)
  ENC := $(ENC_DEF)
endif

# ---- TAG / BASE ----
TAG ?= strict
BASE ?= pinned
ifeq ($(ARCH),x86_64)
ifneq ($(CLANG),)
ifeq ($(BASE),pinned)
  $(warning [make] $(CC) 不支持 -ffixed-r15:x86-64 clang pinned 不可用 → 自动回退 BASE=mat(design.md §5.4/附录 A))
  BASE := mat
endif
endif
endif

# ---- ISA ----
ISA ?= auto
ifeq ($(ISA),auto)
  ifeq ($(ARCH),x86_64)
    ISA := v1
  else
    ISA := armv8-a
  endif
endif

# ---- 变体名派生(CSV variant 列;run_all.sh 可显式覆盖) ----
SUF := $(if $(filter v3,$(ISA)),-v3)$(if $(PURE),-pure)$(if $(SEG),-seg)$(if $(HP),-hp)
ifneq ($(ENC),$(ENC_DEF))
  SUF := -$(ENC)$(SUF)
endif
VARIANT ?= $(CC_ID)-$(TAG)-$(BASE)$(SUF)

# ---- 旗标 ----
OBJDIR  := build/obj/$(notdir $(BIN))
BIN     ?= build/vva_bench
W       ?= 16

CFLAGS   := -std=c11 -O2 -g -Wall -Wextra
# Δins 硬校验前提:禁用自动向量化(见文件头说明;gcc/clang 均接受 tree- 前缀形式)
CFLAGS   += -fno-tree-vectorize -fno-tree-slp-vectorize
CPPFLAGS := -I. -Iinclude -Iarch/$(ARCH) -Ibench -D_GNU_SOURCE

# 编码宏(vva_impl.h 派发)
ifeq ($(ENC),B8)
  CPPFLAGS += -DVVA_ENC_B8
else ifeq ($(ENC),A4)
  CPPFLAGS += -DVVA_ENC_A4
else
  $(error ENC 仅支持 B8 / A4)
endif

# TAG 宏
ifeq ($(TAG),strict)
else ifeq ($(TAG),fast)
  CPPFLAGS += -DVVA_TAG_FAST
else
  $(error TAG 仅支持 strict / fast)
endif

# BASE 宏(mat = 不定义任何 VVA_BASE_*,为 vva_impl.h 的 else 分支)
ifeq ($(BASE),pinned)
  CPPFLAGS += -DVVA_BASE_PINNED
  ifeq ($(ARCH),x86_64)
    CFLAGS += -ffixed-r15
  else
    CFLAGS += -ffixed-x27
  endif
else ifeq ($(BASE),mat)
else ifeq ($(BASE),abs)
  ifeq ($(ARCH),aarch64)
    $(error BASE=abs 仅 x86-64(design.md §5.4))
  endif
  CPPFLAGS += -DVVA_BASE_ABS
  CFLAGS   += -fno-pie -mcmodel=small
  LDFLAGS  += -no-pie
else
  $(error BASE 仅支持 pinned / mat / abs)
endif

# ISA v3(BMI1 bextr;需 CPU 支持,无 BMI 的机器只能编译不能运行)
ifeq ($(ISA),v3)
  ifeq ($(ARCH),aarch64)
    $(error ISA=v3 仅 x86-64)
  endif
  CPPFLAGS += -DVVA_ISA_V3
  CFLAGS   += -march=x86-64-v3
else ifneq ($(ISA),v1)
ifneq ($(ISA),armv8-a)
  $(error ISA 仅支持 v1 / v3(x86)或 armv8-a(AArch64))
endif
endif

# 可选变体宏
ifneq ($(PURE),)
  CPPFLAGS += -DVVA_VARIANT_PURE
endif
ifneq ($(SEG),)
  CPPFLAGS += -DVVA_SEG_CACHE
endif
ifneq ($(HP),)
  CPPFLAGS += -DVVA_TABLE_HP
endif

# 参数头覆盖
ifeq ($(W),)
  W := 16
endif
CPPFLAGS += -DVVA_HIGH_BITS=$(W)
ifneq ($(WMAX),)
  CPPFLAGS += -DVVA_W_MAX=$(WMAX)
endif

# 构建身份宏(bench/main.c 写入 CSV,§7 防张冠李戴)
CPPFLAGS += -DVVA_BUILD_ARCH='"$(ARCH)"' -DVVA_BUILD_ENC='"$(ENC)"' \
            -DVVA_BUILD_TAG='"$(TAG)"' -DVVA_BUILD_BASE='"$(BASE)"' \
            -DVVA_BUILD_ISA='"$(ISA)"' -DVVA_BUILD_W=$(W) \
            -DVVA_BUILD_VARIANT='"$(VARIANT)"'

# ---- 目标 ----
SRCS := main.c harness.c patterns.c kernels.c
OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.c=.o)) $(OBJDIR)/vva_map.o
HDRS := include/vva.h include/vva_common.h arch/$(ARCH)/vva_impl.h \
        bench/harness.h bench/patterns.h bench/kernels.h bench/kernel_defs.h
OBJDUMP ?= objdump

.PHONY: all check codegen clean
all: $(BIN)

vpath %.c bench src

$(OBJDIR) $(dir $(BIN)):
	mkdir -p $@

$(OBJDIR)/%.o: %.c $(HDRS) Makefile | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS) | $(dir $(BIN))
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) -lm

check: $(BIN)
	./$(BIN) --case c0

# 转换循环 codegen 存档(test.md §7:锁定编译器版本并存档 codegen)
codegen: $(BIN)
	@echo "== k_read_c(conv 实例,转换循环) =="
	@$(OBJDUMP) -d $(BIN) --disassemble=k_read_c --no-show-raw-insn | sed -n '/<k_read_c>:/,/^$$/p'
	@echo "== k_read_d(direct 实例,对照) =="
	@$(OBJDUMP) -d $(BIN) --disassemble=k_read_d --no-show-raw-insn | sed -n '/<k_read_d>:/,/^$$/p'

clean:
	rm -rf build
