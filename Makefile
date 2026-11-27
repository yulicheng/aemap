# Makefile — VVA→VA 动态地址映射（核心库 + mock 运行时 + bench + 测试）
#
# 产物：
#   build/libvamap.a    核心库（runtime）
#   build/vamap_bench   测试框架（CLI：--suite correctness / --test / --matrix）
#   build/test_api      API 单元冒烟测试
#
# 编译约束（design.md §5.6 / test.md §9.2）：
#   - -mno-red-zone：融合汇编内含 call，内核不得使用 red zone；
#   - 融合汇编的慢路径 call 在 asm 内显式做 rsp/sp 16 字节对齐（编译器把
#     volatile asm 视作 leaf，不保证 call 点对齐）；
#   - bench 使用 -O2 -fno-tree-vectorize，避免 B0/B1 自动向量化差异破坏
#     instr_per_op 断言。

CC      ?= gcc
AR      ?= ar

ARCH := $(shell uname -m)

BASEFLAGS := -O2 -std=c11 -Wall -Wextra -pthread \
             -fno-semantic-interposition -fno-tree-vectorize -fno-strict-aliasing

# 融合汇编含 call：一律禁 red zone
CFLAGS_ARCH := -mno-red-zone

BUILD_TAG := strict
ifeq ($(VAMAP_FAST_TAG0),1)
  BASEFLAGS += -DVAMAP_FAST_TAG0=1
  BUILD_TAG := fast-tag0
endif

BUILD := build
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
	scripts/check_fastpath.sh $(BUILD)/vamap_bench

clean:
	rm -rf $(BUILD) results
