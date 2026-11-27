# Makefile — VVA→VA 动态地址映射（核心库 + mock 运行时 + bench + 测试）
#
# 产物（BUILD = build/<VARIANT>）：
#   $(BUILD)/libvamap.a    核心库（runtime）
#   $(BUILD)/vamap_bench   测试框架（CLI：--suite correctness / --test / --matrix）
#   $(BUILD)/test_api      API 单元冒烟测试
#
# 构建变体（test.md §3.1，design.md §5.2/§5.3）：
#   make VARIANT=strict      x86-64 / aarch64 通用默认（strict 掩码形态）
#   make VARIANT=fast-tag0   tag≡0 约定（省 andl / idx 掩码不变）
#   make VARIANT=bmi2        仅 x86-64：BMI2 shrx（strict 7 / fast 6）
#   make VARIANT=sve2-batch  仅 aarch64：SVE2 批量（每批 fast 6 / strict 7）
# BUILD 同时含目标架构与变体：native 与交叉产物互不覆盖，切换 CC 不会
# 因 .o 时间戳而静默复用错误架构的对象。
#
# 编译约束（design.md §5.6 / test.md §9.2）：
#   - -mno-red-zone（仅 x86-64）：融合汇编内含 call，内核不得使用 red zone；
#   - 融合汇编的慢路径 call 在 asm 内显式做 rsp 16 字节对齐（aarch64 由
#     AAPCS 保证 sp 恒对齐，无需处理）；
#   - bench 使用 -O2 -fno-tree-vectorize，避免 B0/B1 自动向量化差异破坏
#     instr_per_op 断言。

CC      ?= gcc
AR      ?= ar

VARIANT ?= strict

VALID_VARIANTS := strict fast-tag0 bmi2 sve2-batch
ifeq ($(filter $(VARIANT),$(VALID_VARIANTS)),)
  $(error VARIANT 必须是 strict / fast-tag0 / bmi2 / sve2-batch 之一)
endif

# 目标架构（随 CC 变化，支持交叉编译，如 aarch64-linux-gnu-gcc）
TARGET_ARCH := $(strip \
  $(if $(findstring aarch64,$(shell $(CC) -dumpmachine)),aarch64, \
  $(if $(findstring x86_64,$(shell $(CC) -dumpmachine)),x86_64,unknown)))

BASEFLAGS := -O2 -std=c11 -Wall -Wextra -pthread \
             -fno-semantic-interposition -fno-tree-vectorize -fno-strict-aliasing

# x86-64：融合汇编含 call，一律禁 red zone；aarch64 无该选项（AAPCS 保证
# sp 恒 16 字节对齐），传了会报 unrecognized command-line option
ifeq ($(TARGET_ARCH),x86_64)
  CFLAGS_ARCH := -mno-red-zone
else
  CFLAGS_ARCH :=
endif

ifeq ($(VARIANT),fast-tag0)
  BASEFLAGS += -DVAMAP_FAST_TAG0=1
else ifeq ($(VARIANT),bmi2)
  ifneq ($(TARGET_ARCH),x86_64)
    $(error bmi2 变体仅支持 x86-64)
  endif
  BASEFLAGS += -DVAMAP_BMI2=1 -mbmi2
else ifeq ($(VARIANT),sve2-batch)
  ifneq ($(TARGET_ARCH),aarch64)
    $(error sve2-batch 变体仅支持 aarch64（ARMv9 + SVE2）)
  endif
  BASEFLAGS += -DVAMAP_SVE2_BATCH=1 -march=armv9-a+sve2
endif

BUILD := build/$(TARGET_ARCH)-$(VARIANT)
INC    := -Iinclude -Ibench

LIB_SRC   := src/vamap_runtime.c
BENCH_SRC := bench/main.c bench/patterns.c bench/timer.c bench/correctness.c
TEST_SRC  := tests/test_api.c

.PHONY: all clean bench test correctness smoke check-fastpath

all: $(BUILD)/libvamap.a $(BUILD)/vamap_bench $(BUILD)/test_api

$(BUILD) $(BUILD)/bench $(BUILD)/tests:
	mkdir -p $@

$(BUILD)/vamap_runtime.o: src/vamap_runtime.c include/vamap.h | $(BUILD)
	$(CC) $(BASEFLAGS) $(CFLAGS_ARCH) $(INC) -c $< -o $@

$(BUILD)/%.o: %.c include/vamap.h bench/bench.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(BASEFLAGS) $(CFLAGS_ARCH) $(INC) -c $< -o $@

$(BUILD)/libvamap.a: $(BUILD)/vamap_runtime.o
	$(AR) rcs $@ $^

$(BUILD)/vamap_bench: $(BENCH_SRC:%.c=$(BUILD)/%.o) $(BUILD)/libvamap.a
	$(CC) $(BASEFLAGS) $(CFLAGS_ARCH) -Iinclude -o $@ $^ -lm

$(BUILD)/test_api: $(TEST_SRC:%.c=$(BUILD)/%.o) $(BUILD)/libvamap.a
	$(CC) $(BASEFLAGS) $(CFLAGS_ARCH) -Iinclude -o $@ $^ -lm

bench: $(BUILD)/vamap_bench
test: $(BUILD)/test_api

correctness: $(BUILD)/vamap_bench
	$(BUILD)/vamap_bench --suite correctness

smoke: $(BUILD)/vamap_bench $(BUILD)/test_api
	$(BUILD)/test_api && $(BUILD)/vamap_bench --matrix --level smoke --out results/smoke/

check-fastpath: $(BUILD)/vamap_bench
	scripts/check_fastpath.sh $(BUILD)/vamap_bench $(VARIANT)

clean:
	rm -rf build results
