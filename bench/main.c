/* bench/main.c —— CLI 与用例调度(test.md §2.2/§4)
 *
 * 用法:
 *   vva_bench --case c0                       # 差分正确性 gate
 *   vva_bench --case c4 --k 4096 --reps 30 --csv out.csv
 *   vva_bench --case c13                      # C13 便捷别名(当前构建变体跑 c2 + c4 扫描)
 *
 * 公平性(§1.1 强制):direct/conv 访问完全相同的物理 cache line ——
 * direct 列表 = vva2va_pure(conv 列表),每个用例运行前逐元素断言。
 * 统计协议(§3.6):2 次预热 + N 次重复,direct/conv ABBA 交错成对做差,
 * 报告中位数 ± MAD。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "vva.h"
#include "harness.h"
#include "patterns.h"
#include "kernels.h"

/* ---------------- 构建身份(Makefile 注入;手工编译时给出合理回退) ---------------- */
#ifndef VVA_BUILD_ARCH
#if defined(__x86_64__)
#define VVA_BUILD_ARCH "x86_64"
#elif defined(__aarch64__)
#define VVA_BUILD_ARCH "aarch64"
#else
#define VVA_BUILD_ARCH "?"
#endif
#endif
#ifndef VVA_BUILD_ENC
#define VVA_BUILD_ENC (VVA_ENC_A4 ? "A4" : "B8")
#endif
#ifndef VVA_BUILD_TAG
#define VVA_BUILD_TAG (VVA_TAG_FAST ? "fast" : "strict")
#endif
#ifndef VVA_BUILD_BASE
#if defined(VVA_BASE_PINNED)
#define VVA_BUILD_BASE "pinned"
#elif defined(VVA_BASE_ABS)
#define VVA_BUILD_BASE "abs"
#else
#define VVA_BUILD_BASE "mat"
#endif
#endif
#ifndef VVA_BUILD_ISA
#if defined(VVA_ISA_V3)
#define VVA_BUILD_ISA "v3"
#elif defined(__aarch64__)
#define VVA_BUILD_ISA "armv8-a"
#else
#define VVA_BUILD_ISA "v1"
#endif
#endif
#ifndef VVA_BUILD_W
#define VVA_BUILD_W 16
#endif
#ifndef VVA_BUILD_VARIANT
#define VVA_BUILD_VARIANT "default"
#endif

/* fast 契约标记静态检查(test.md C0-3):fast 构建必须由实现头提供契约注释标记 */
#if defined(VVA_TAG_FAST) && !defined(VVA_TAG_CONTRACT_ZERO)
#error "TAG=fast 构建缺少 VVA_TAG_CONTRACT_ZERO 契约标记(design.md §3.2/§4.2)"
#endif

#if defined(__x86_64__)
#define G_ARCH_IS_X86 1
#else
#define G_ARCH_IS_X86 0
#endif

/* 生成器/用例共用的对齐辅助(patterns.c 内同款) */
static uint64_t align4(uint64_t v) { return v & ~(uint64_t)3; }
static uint64_t align8(uint64_t v) { return v & ~(uint64_t)7; }

/* kernels.c 定义(防 DCE sink / C6 冷表钩子;见 kernel_defs.h) */
extern volatile uint64_t g_sink;
extern void (*volatile g_hop_hook)(void);

/* ============================ 选项 ============================ */

struct opts {
    const char *cases;      /* 逗号分隔:c0..c14 */
    long   k, nops, reps, bytes, window, span, stride, win_bytes, block, mb;
    const char *sub;        /* 用例子模式 */
    long   seed;
    long   llc_mb;
    const char *flush;      /* auto / cl / evict */
    long   raw_l2m;         /* <0 关闭;否则 raw config */
    const char *csv;
    const char *arch, *enc, *tag, *base, *isa, *variant;
    int    force, no_ins_check, table_hp, list_only;
};

static struct opts O = {
    .cases = "c0", .reps = 30, .k = 0, .nops = 0, .bytes = 0, .window = 0,
    .span = 0, .stride = 0, .win_bytes = 0, .block = 0, .mb = 0,
    .seed = 0x9E3779B97F4A7C15LL, .llc_mb = 96, .flush = "auto",
#if defined(__aarch64__)
    .raw_l2m = 0x17,          /* PMUv3 架构化 L2D_CACHE_REFILL(§3.2) */
#else
    .raw_l2m = -1,            /* Intel raw umask 因微架构而异,--raw-l2m 显式开启 */
#endif
    .sub = "", .variant = VVA_BUILD_VARIANT,
};

/* ============================ 运行上下文 ============================ */

#define MAX_MET 6

struct runctx {
    struct pat_ctx pat;
    kern_fn kd, kc;
    uint64_t p1d, p1c;        /* p1:拷贝目的列表 / daxpy y / matmul B(按模式取) */
    uint64_t p2d, p2c;        /* p2:matmul C 基址等按模式参数(其余两模式同值) */
    size_t n_ops;             /* 归一化分母:访存次数 / 拷贝字节 / FLOP */
    uint32_t K;               /* 映射表条目数 [0,K) */
    int flush_table;          /* C5:每轮重复前 table_flush */
    int data_cold;            /* 每轮 evict_llc(随后重热表) */
    int refill_chain;         /* C6:每轮按模式重填链值 */
    const char *param;
    int is_loop_read;         /* 内核为单转换循环(可做 Δins 硬校验) */
};

/* C6 冷表钩子:每跳 flush 表行(direct 侧同样执行,成本对称,§4.7)。
 * 钩子无参,flush 范围由用例装配时写入 g_hop_flush_K。 */
static uint32_t g_hop_flush_K;
static void hop_flush_hook(void) { table_flush(g_hop_flush_K); }

/* ---------------- 数据段:(1<<32) 起 4GB 对齐,idx→H 多对一 ---------------- */

static uint8_t *mmap_data(size_t size, int populate) {
    for (int t = 0; t < 8; t++) {
        void *hint = (void *)((uint64_t)(t + 1) << 32);
        int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE
                  | (populate ? MAP_POPULATE : 0);
        void *p = mmap(hint, size, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (p == MAP_FAILED) break;
        /* 4GB 对齐:off↔VA 关系成立的前提;且 H 须落在 W 上限内 */
        if (((uintptr_t)p & 0xFFFFFFFFULL) == 0 &&
            ((uintptr_t)p >> 32) < (1u << VVA_W_MAX))
            return p;
        munmap(p, size);
    }
    fprintf(stderr, "mmap_data: 无法获得 4GB 对齐数据段(H 受 W 上限约束)\n");
    return NULL;
}

static int setup_map(struct runctx *rc, uint8_t *base) {
    uint64_t h = (uintptr_t)base >> 32;
    uint32_t wmax = VVA_HIGH_BITS < VVA_W_MAX ? VVA_HIGH_BITS : VVA_W_MAX;
    if ((uintptr_t)base & 0xFFFFFFFFULL) {
        fprintf(stderr, "数据段非 4GB 对齐,映射关系不成立\n");
        return -1;
    }
    if (h >= (1u << wmax)) {
        fprintf(stderr, "数据段高 %llu 位超出 W=%u 上限\n",
                (unsigned long long)h, wmax);
        return -1;
    }
    vva_map_init();
    for (uint32_t i = 0; i < rc->K; i++)
        vva_map_set(i, (uint32_t)h);      /* 多对一(§3.5);idx0 亦映射,C0 另测哨兵 */
    table_warm(rc->K);
    return 0;
}

/* 公平性断言(§4.1-5):vva2va(conv[i]) == direct[i] 逐元素 */
static int check_fairness(const struct pat_ctx *c) {
    size_t step = c->n > 1000000 ? c->n / 1000000 : 1;
    for (size_t i = 0; i < c->n; i += step)
        if ((uint64_t)(uintptr_t)vva2va(c->conv[i]) != c->direct[i]) {
            fprintf(stderr, "公平性断言失败 @%zu\n", i);
            return -1;
        }
    if (c->dst_conv) {
        size_t n = c->n;
        for (size_t i = 0; i < n; i += step)
            if ((uint64_t)(uintptr_t)vva2va(c->dst_conv[i]) != c->dst_direct[i]) {
                fprintf(stderr, "公平性断言失败(dst)@%zu\n", i);
                return -1;
            }
    }
    return 0;
}

/* ---------------- 状态控制(§3.4) ---------------- */

static void prep_state(struct runctx *rc, int conv_mode) {
    if (rc->data_cold) {
        evict_llc();
        table_warm(rc->K);            /* evict 连表一起驱逐,重热表侧 */
    }
    if (rc->flush_table)
        table_flush(rc->K);
    if (rc->refill_chain)
        fill_chain(&rc->pat, conv_mode);
}

/* ---------------- 计时 + 报告 ---------------- */

struct rep_stats { double cyc, ins, l1m, llcm, dtlb, l2m; };

static void kern_run(struct runctx *rc, int conv_mode, struct rep_stats *st,
                     struct pmuset *pmu) {
    kern_fn k = conv_mode ? rc->kc : rc->kd;
    const uint64_t *addr = conv_mode ? rc->pat.conv : rc->pat.direct;
    uint64_t p1 = conv_mode ? rc->p1c : rc->p1d;
    uint64_t p2 = conv_mode ? rc->p2c : rc->p2d;

    if (!pmu->ok) {
        uint64_t t0 = tsc_now();
        k(addr, rc->pat.n, p1, p2);
        uint64_t t1 = tsc_now();
        st->cyc = (double)(t1 - t0);
        return;
    }
    pmu_reset(pmu);
    pmu_start(pmu);
    k(addr, rc->pat.n, p1, p2);
    pmu_stop(pmu);
    pmu_read(pmu);
    st->cyc  = (double)pmu->val[PMU_CYC];
    st->ins  = (double)pmu->val[PMU_INS];
    st->l1m  = (double)pmu->val[PMU_L1M];
    st->llcm = (double)pmu->val[PMU_LLCM];
    st->dtlb = (double)pmu->val[PMU_DTLB];
    st->l2m  = (double)pmu->val[PMU_L2M];
}

/* c8 带宽参考值:n_ops(拷贝字节数)/ 秒,由实测频率换算。
 * 仅 PMU cycles 有效(rdtsc 回退是墙钟 TSC,频率不同,不可换算)。
 * 最终报告带宽由 analyze.py 从 CSV(freq_mhz 列)统一计算,此处仅即时参考。 */
static double bytes_gbs(const double *v, long reps, struct runctx *rc,
                        struct pmuset *pmu) {
    if (!pmu->ok) return 0.0;
    double mhz = cpu_freq_mhz();
    if (mhz <= 0.0) return 0.0;
    double cyc = median_of(v, reps);
    if (cyc <= 0.0) return 0.0;
    return (double)rc->n_ops * mhz * 1e6 / cyc / 1e9;   /* 字节 × MHz / cycles → GB/s */
}

static int run_and_report(struct runctx *rc, const char *case_id, struct opts *o,
                          struct pmuset *pmu, FILE *csvf) {
    long reps = o->reps;
    struct rep_stats *ds = calloc(reps, sizeof(*ds));
    struct rep_stats *cs = calloc(reps, sizeof(*cs));
    double *diff[MAX_MET], *dv[MAX_MET], *cv[MAX_MET];
    for (int m = 0; m < MAX_MET; m++) {
        diff[m] = malloc(reps * sizeof(double));
        dv[m] = malloc(reps * sizeof(double));
        cv[m] = malloc(reps * sizeof(double));
    }
    static const char *met_name[MAX_MET] = { "cyc", "ins", "l1m", "llcm", "dtlb", "l2m" };
    (void)met_name;

    /* 2 次预热(§3.6):两模式各一次 */
    for (int w = 0; w < 2; w++) {
        int cm = w & 1;
        prep_state(rc, cm);
        struct rep_stats tmp; memset(&tmp, 0, sizeof(tmp));
        kern_run(rc, cm, &tmp, pmu);
    }
    g_sink = 0;

    /* ABBA 交错:偶数轮 direct 先、奇数轮 conv 先,成对做差 */
    for (long r = 0; r < reps; r++) {
        int first_conv = (r & 1);
        prep_state(rc, first_conv);
        kern_run(rc, first_conv, first_conv ? &cs[r] : &ds[r], pmu);
        prep_state(rc, !first_conv);
        kern_run(rc, !first_conv, first_conv ? &ds[r] : &cs[r], pmu);
    }

    /* Δ = conv − direct,按轮配对;报告中位数 ± MAD */
    double med[MAX_MET], mad[MAX_MET], dmed_op[MAX_MET];
    for (int m = 0; m < MAX_MET; m++) {
        for (long r = 0; r < reps; r++) {
            double d = (m == 0 ? cs[r].cyc : m == 1 ? cs[r].ins : m == 2 ? cs[r].l1m :
                        m == 3 ? cs[r].llcm : m == 4 ? cs[r].dtlb : cs[r].l2m);
            double e = (m == 0 ? ds[r].cyc : m == 1 ? ds[r].ins : m == 2 ? ds[r].l1m :
                        m == 3 ? ds[r].llcm : m == 4 ? ds[r].dtlb : ds[r].l2m);
            diff[m][r] = d - e;
            dv[m][r] = e;
            cv[m][r] = d;
        }
        med[m] = median_of(diff[m], reps);
        mad[m] = mad_of(diff[m], reps, med[m]);
        dmed_op[m] = rc->n_ops ? med[m] / (double)rc->n_ops : 0.0;
    }

    /* Δins/op 硬校验(design §8.1:逐指令对应,±0;仅单转换循环内核启用) */
    long expect = VVA_CONV_INSN;
    int ins_ok = -1;
    if (rc->is_loop_read && pmu->ok && expect > 0 && !o->no_ins_check) {
        double dins_op = dmed_op[1];
        ins_ok = (dins_op == (double)expect) ? 1 : 0;
    }

    /* ---- 人类可读输出 ---- */
    double dcyc_op = rc->n_ops ? median_of(dv[0], reps) / (double)rc->n_ops : 0.0;
    double ccyc_op = rc->n_ops ? median_of(cv[0], reps) / (double)rc->n_ops : 0.0;
    double mad_ratio = med[0] != 0 ? mad[0] / (med[0] > 0 ? med[0] : -med[0]) * 100.0 : 0;
    printf("[%s] %s reps=%ld n_ops=%zu K=%u%s\n", case_id, rc->param, reps, rc->n_ops,
           rc->K, rc->flush_table ? " +table_flush" : "");
    printf("  Δcyc/op = %+8.2f ± %.2f   (direct %8.2f → conv %8.2f, mad/med %4.1f%%)%s\n",
           dmed_op[0], rc->n_ops ? mad[0] / (double)rc->n_ops : 0.0, dcyc_op, ccyc_op,
           mad_ratio, mad_ratio > 5.0 ? "  ⚠ 不稳定" : "");
    if (ins_ok >= 0)
        printf("  Δins/op = %7.3f   (期望 %ld)%s\n", dmed_op[1], expect,
               ins_ok ? "  ✓" : "  ✗ FAIL");
    else
        printf("  Δins/op = %7.3f   (n/a)\n", dmed_op[1]);
    printf("  Δl1m/op = %+.2e  Δllcm/op = %+.2e  Δdtlb/op = %+.2e  Δl2m/op = %+.2e\n",
           dmed_op[2], dmed_op[3], dmed_op[4], dmed_op[5]);
    if (strstr(rc->param, "bytes") && rc->n_ops)
        printf("  带宽参考:direct %.2f GB/s → conv %.2f GB/s(Δ%+.1f%%)\n",
               bytes_gbs(dv[0], reps, rc, pmu),
               bytes_gbs(cv[0], reps, rc, pmu),
               dcyc_op > 0 ? (ccyc_op / dcyc_op - 1.0) * 100.0 : 0.0);

    /* ---- CSV(test.md §5.1 + 扩展列:pmu,mad_cyc_op,ins_expect,ins_ok) ---- */
    if (csvf) {
        fprintf(csvf,
                "%s,%s,%s,%s,%s,%s,%d,\"%s\",\"%s\",%ld,"
                "%.1f,%.1f,%zu,%.1f,%.1f,"
                "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
                "%.1f,%.1f,%.1f,%d,%.3f,%ld,%d\n",
                VVA_BUILD_ARCH, o->variant, VVA_BUILD_ENC, VVA_BUILD_TAG, VVA_BUILD_BASE,
                VVA_BUILD_ISA, VVA_BUILD_W, case_id, rc->param, reps,
                median_of(dv[0], reps), median_of(cv[0], reps), rc->n_ops,
                median_of(dv[1], reps), median_of(cv[1], reps),
                median_of(dv[2], reps), median_of(cv[2], reps),
                median_of(dv[5], reps), median_of(cv[5], reps),
                median_of(dv[3], reps), median_of(cv[3], reps),
                median_of(dv[4], reps), median_of(cv[4], reps),
                cpu_freq_mhz(), pmu->ok, mad[0] / (double)(rc->n_ops ? rc->n_ops : 1),
                expect, ins_ok);
        fflush(csvf);
    }

    for (int m = 0; m < MAX_MET; m++) { free(diff[m]); free(dv[m]); free(cv[m]); }
    free(ds); free(cs);
    return (ins_ok == 0) ? 2 : 0;
}

/* ============================ C0 差分正确性(§4.1) ============================ */

static int c0_fail(const char *what) {
    fprintf(stderr, "C0 FAIL: %s\n", what);
    return 2;
}

static int run_c0(struct opts *o) {
    (void)o;
    printf("== C0 差分正确性(arch=%s enc=%s tag=%s base=%s isa=%s W=%d) ==\n",
           VVA_BUILD_ARCH, VVA_BUILD_ENC, VVA_BUILD_TAG, VVA_BUILD_BASE,
           VVA_BUILD_ISA, VVA_BUILD_W);

    /* 4) entry0 哨兵(§1.5):全零表 → idx0 转换结果 = off(落低端 4GB) */
    vva_map_init();
    if ((uintptr_t)vva2va(VVA_MAKE(0, 0x12345678u)) != 0x12345678u)
        return c0_fail("entry0 哨兵:map[0]==0 应转换出低端 4GB 地址");
    if ((uintptr_t)vva2va_pure(VVA_MAKE(0, 0x9abcdef0u)) != 0x9abcdef0u)
        return c0_fail("entry0 哨兵(pure)");

    /* 数据段 + 4K 项多对一映射 */
    enum { DATA_BYTES = 4u << 20, K0 = 4096, NOPS = 1 << 20 };
    uint8_t *base = mmap_data(DATA_BYTES, 1);
    if (!base) return c0_fail("mmap 数据段");
    uint64_t h = (uintptr_t)base >> 32;
    struct runctx rc; memset(&rc, 0, sizeof(rc));
    rc.K = K0;
    if (setup_map(&rc, base)) return c0_fail("映射建立");
    uint32_t wmax = VVA_HIGH_BITS < VVA_W_MAX ? VVA_HIGH_BITS : VVA_W_MAX;

    /* 1) 合法 H 边界可写入:仅校验编码存储,不解引用(该 H 无真实映射),
     *    测完立即恢复 idx7 → 数据段,后续随机访问才安全 */
    vva_map_set(7, (1u << wmax) - 1);
    if ((uintptr_t)vva2va(VVA_MAKE(7, 0x40)) !=
        ((((uint64_t)((1u << wmax) - 1)) << 32) | 0x40))
        return c0_fail("最大合法 H 未按编码存储");
    vva_map_set(7, (uint32_t)h);

    /* 2) 1e6 随机 LD/ST(写后读回 + golden 对照) */
    uint32_t *golden = calloc(NOPS, 4);
    uint64_t rng = (uint64_t)O.seed ^ 0xC0FFEE;
    for (long i = 0; i < NOPS; i++) {
        uint32_t idx = (uint32_t)(xs64(&rng) % K0);
        uint32_t off = (uint32_t)(align4(xs64(&rng)) % DATA_BYTES);
        uint32_t v = (uint32_t)xs64(&rng);
        uint64_t vva = VVA_MAKE(idx, off);
        VVA_ST(uint32_t, vva, v);                    /* conv 写 */
        golden[off >> 2] = v;
    }
    for (long i = 0; i < NOPS; i++) {
        uint32_t idx = (uint32_t)(xs64(&rng) % K0);
        uint32_t off = (uint32_t)(align4(xs64(&rng)) % DATA_BYTES);
        uint64_t vva = VVA_MAKE(idx, off);
        if (VVA_LD(uint32_t, vva) != golden[off >> 2])   /* conv 读 ≡ golden */
            return c0_fail("conv 读值与 golden 不一致");
        if (*(uint32_t *)(base + off) != golden[off >> 2]) /* 直接读回(conv 写落点) */
            return c0_fail("写后直接读回不一致");
    }

    /* 3) tag 不变性 + asm≡pure:1e6 随机(strict:tag∈[0,2^12);fast 契约下 tag≡0) */
    for (long i = 0; i < NOPS; i++) {
        uint32_t idx = (uint32_t)(xs64(&rng) % K0);
        uint32_t off = (uint32_t)(align4(xs64(&rng)) % DATA_BYTES);
#if defined(VVA_TAG_FAST)
        uint64_t tag = 0;                            /* fast:tag≡0 契约 */
#else
        uint64_t tag = xs64(&rng) & 0xFFF;           /* strict:tag 任意 */
#endif
        uint64_t a = VVA_MAKE(idx, off) | (tag << 52);
        uint64_t z = VVA_MAKE(idx, off);             /* tag≡0 参照 */
        uint64_t va_a = (uintptr_t)vva2va(a);
        if (va_a != (uintptr_t)vva2va_pure(a))
            return c0_fail("asm 与纯 C 参考不一致");
        if (va_a != (uintptr_t)vva2va(z))
            return c0_fail("tag 不变性违例");
    }

    /* 5) 对齐:转换不改变原指针 8 字节对齐属性 */
    for (long i = 0; i < 100000; i++) {
        uint64_t off = align8(xs64(&rng)) % DATA_BYTES;
        uint64_t vva = VVA_MAKE((uint32_t)(xs64(&rng) % K0), off);
        if (((uintptr_t)vva2va(vva) & 7) != (off & 7))
            return c0_fail("对齐保持违例");
    }

    /* 6) W 上限 / idx 越界断言:fork 验证 abort */
    for (int t = 0; t < 2; t++) {
        pid_t pid = fork();
        if (pid == 0) {
            if (t == 0) vva_map_set(7, 1u << wmax);        /* H 越界 → abort */
            else        vva_map_set(VVA_MAP_SIZE, 1);      /* idx 越界 → abort */
            _exit(0);
        }
        int st; waitpid(pid, &st, 0);
        if (!(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT))
            return c0_fail(t == 0 ? "H 越界未断言" : "idx 越界未断言");
    }

    /* 7) 公平性(§1.1):本用例列表逐元素相等 */
    struct pat_ctx pc; memset(&pc, 0, sizeof(pc));
    pc.conv = malloc(NOPS * 8); pc.direct = malloc(NOPS * 8);
    pc.data = base; pc.K = K0; pc.n = NOPS;
    for (long i = 0; i < NOPS; i++)
        pc.conv[i] = VVA_MAKE((uint32_t)(xs64(&rng) % K0),
                              (uint32_t)(align4(xs64(&rng)) % DATA_BYTES));
    derive_direct(&pc);
    if (check_fairness(&pc)) return c0_fail("direct/conv 地址列表不一致");
    free(pc.conv); free(pc.direct);

    free(golden);
    munmap(base, DATA_BYTES);
    printf("C0 PASS(1e6 LD/ST + tag/对齐/哨兵/断言/公平性 全部通过)\n");
    return 0;
}

/* ============================ 用例装配 ============================ */

static int case_run(const char *id, struct opts *o, struct pmuset *pmu, FILE *csvf);

static void free_pat(struct pat_ctx *p) {
    chain_free(p);
    free(p->conv); free(p->direct);
    p->conv = p->direct = NULL;
    p->dst_conv = p->dst_direct = NULL;
}

static int case_run(const char *id, struct opts *o, struct pmuset *pmu, FILE *csvf) {
    if (!strcmp(id, "c0")) return run_c0(o);

    struct runctx rc; memset(&rc, 0, sizeof(rc));
    struct pat_ctx *p = &rc.pat;
    p->seed = (uint64_t)O.seed ^ (uint64_t)(uintptr_t)id;
    rc.is_loop_read = 1;
    char param[96]; rc.param = param;

    size_t n = o->nops > 0 ? (size_t)o->nops : 0;
    uint64_t bytes = 0, span_v = 0;
    int copy_word = 0, hp_data = 0;

    if (!strcmp(id, "c1")) {                       /* 顺序流(§4.2) */
        bytes = o->bytes > 0 ? (uint64_t)o->bytes : (4u << 20);
        if (!n) n = bytes / 4;
        rc.K = 2;
        int wr = o->sub && (!strcmp(o->sub, "write") || !strcmp(o->sub, "w"));
        rc.kd = kern_get(wr ? K_WRITE : K_READ, 0);
        rc.kc = kern_get(wr ? K_WRITE : K_READ, 1);
        snprintf(param, sizeof(param), "bytes=%llu,rw=%s", (unsigned long long)bytes, wr ? "w" : "r");
    } else if (!strcmp(id, "c2")) {                /* 相近 VVA 热点(§4.3,必测) */
        uint32_t win = o->window > 0 ? (uint32_t)o->window : 32;
        if (!n) n = 2u << 20;
        rc.K = 2;
        rc.kd = kern_get(K_READ, 0); rc.kc = kern_get(K_READ, 1);
        snprintf(param, sizeof(param), "window=%u", win);
        p->window = win;
    } else if (!strcmp(id, "c3")) {                /* 段内随机(§4.4) */
        span_v = o->span > 0 ? (uint64_t)o->span : (64u << 10);
        if (!n) n = 2u << 20;
        rc.K = 2;
        rc.kd = kern_get(K_READ, 0); rc.kc = kern_get(K_READ, 1);
        snprintf(param, sizeof(param), "span=%llu", (unsigned long long)span_v);
    } else if (!strcmp(id, "c4") || !strcmp(id, "c4b")) { /* 随机索引(§4.5,必测) */
        uint32_t K = o->k > 0 ? (uint32_t)o->k : 4096;
        rc.K = K;
        p->K = K;
        p->independent = (id[2] == 'b');
        if (!n) n = K <= 4096 ? (2u << 20) : (K <= 65536 ? (1u << 20) : (500u << 10));
        rc.kd = kern_get(K_READ, 0); rc.kc = kern_get(K_READ, 1);
        snprintf(param, sizeof(param), "k=%u%s", K, p->independent ? ",indep=1" : "");
    } else if (!strcmp(id, "c5")) {                /* 表全冷(§4.6) */
        /* 默认 K=1M:表足迹 8MB(B8)>> LLC,flush 后整个 rep 内表线保持冷,
         * Δ/op 才是"一次表线访存延迟"量级(≈ C4 DRAM 段,两法互为校验);
         * K 很小时表会在 rep 内被重热,Δ 被稀释。--k 可覆盖。 */
        uint32_t K = o->k > 0 ? (uint32_t)o->k : 1048576;
        rc.K = K;
        p->K = K;
        rc.flush_table = 1;
        if (!n) n = K <= 4096 ? (500u << 10) : (250u << 10);
        rc.kd = kern_get(K_READ, 0); rc.kc = kern_get(K_READ, 1);
        snprintf(param, sizeof(param), "k=%u,flush=1", K);
    } else if (!strcmp(id, "c6")) {                /* 指针追逐(§4.7) */
        /* 冷表:常数 idx + 每跳 flush 表行(g_hop_hook,两模式对称)。
         * test.md 还提到"随机 idx",但冷表关键路径由每跳 flush 完整复制
         * (任一 idx 的表线均为冷),随机 idx 不改变延迟结构,故省略。 */
        size_t hops = n ? n : (1u << 20);
        rc.K = 2;
        rc.refill_chain = 1;
        rc.is_loop_read = 0;
        if (o->sub && !strcmp(o->sub, "cold")) {
            g_hop_flush_K = rc.K;
            g_hop_hook = hop_flush_hook;
        }
        rc.kd = kern_get(K_CHASE, 0); rc.kc = kern_get(K_CHASE, 1);
        snprintf(param, sizeof(param), "hops=%zu,%s", hops,
                 g_hop_hook ? "cold" : "hot");
        n = hops;
    } else if (!strcmp(id, "c7")) {                /* 跨段步长(§4.8) */
        uint32_t K = o->k > 0 ? (uint32_t)o->k : 65536;
        uint32_t st = o->stride > 0 ? (uint32_t)o->stride : 1;
        if (!n) n = 2u << 20;
        rc.K = K; p->K = K; p->stride = st;
        rc.kd = kern_get(K_READ, 0); rc.kc = kern_get(K_READ, 1);
        snprintf(param, sizeof(param), "k=%u,stride=%u", K, st);
    } else if (!strcmp(id, "c8")) {                /* 拷贝带宽(§4.9) */
        bytes = o->bytes > 0 ? (uint64_t)o->bytes : (16u << 20);
        uint32_t bw = o->block > 0 ? (uint32_t)(o->block / 4) : 16;
        copy_word = o->sub && !strcmp(o->sub, "word");
        rc.is_loop_read = 0;
        enum kern_id kk = (o->sub && !strcmp(o->sub, "nt")) ? K_COPY_NT
                        : copy_word ? K_COPY_WORD : K_COPY_BLK;
        rc.K = 2;
        rc.kd = kern_get(kk, 0); rc.kc = kern_get(kk, 1);
        rc.p2d = rc.p2c = copy_word ? 0 : bw;
        rc.n_ops = bytes;                          /* 归一化 = 拷贝数据量 */
        snprintf(param, sizeof(param), "bytes=%llu,block=%u,mode=%s",
                 (unsigned long long)bytes, copy_word ? 4 : bw * 4,
                 (o->sub && !strcmp(o->sub, "nt")) ? "nt" : copy_word ? "word" : "blk");
        p->block_words = bw;
    } else if (!strcmp(id, "c9")) {                /* gather/scatter(§4.10) */
        uint32_t K = o->k > 0 ? (uint32_t)o->k : 4096;
        uint32_t win = o->win_bytes > 0 ? (uint32_t)o->win_bytes : 65536;
        if (!n) n = 1u << 20;
        rc.K = K; p->K = K; p->win_bytes = win;
        rc.kd = kern_get(K_READ, 0); rc.kc = kern_get(K_READ, 1);
        snprintf(param, sizeof(param), "k=%u,win=%u", K, win);
    } else if (!strcmp(id, "c10")) {               /* Zipf(§4.11) */
        uint32_t K = o->k > 0 ? (uint32_t)o->k : 65536;
        if (!n) n = 1u << 20;
        rc.K = K; p->K = K;
        rc.kd = kern_get(K_READ, 0); rc.kc = kern_get(K_READ, 1);
        snprintf(param, sizeof(param), "k=%u,zipf=0.9", K);
    } else if (!strcmp(id, "c11")) {               /* 读写混合(§4.12) */
        span_v = o->span > 0 ? (uint64_t)o->span : (1u << 20);
        if (!n) n = 1u << 20;
        enum kern_id kk = K_READ;
        const char *md = "r";
        if (o->sub && (!strcmp(o->sub, "w") || !strcmp(o->sub, "W"))) { kk = K_WRITE; md = "w"; }
        else if (o->sub && (!strcmp(o->sub, "rmw") || !strcmp(o->sub, "RMW"))) { kk = K_RMW; md = "rmw"; }
        rc.K = 2;
        rc.kd = kern_get(kk, 0); rc.kc = kern_get(kk, 1);
        snprintf(param, sizeof(param), "span=%llu,mode=%s", (unsigned long long)span_v, md);
    } else if (!strcmp(id, "c12")) {               /* TLB 交互(§4.13) */
        uint64_t mb = o->mb > 0 ? (uint64_t)o->mb : 256;
        int hp = o->sub && (!strcmp(o->sub, "2m") || !strcmp(o->sub, "2M"));
        if (!n) n = 1u << 20;
        rc.K = 2;
        rc.kd = kern_get(K_READ, 0); rc.kc = kern_get(K_READ, 1);
        snprintf(param, sizeof(param), "mb=%llu,page=%s", (unsigned long long)mb, hp ? "2m" : "4k");
        span_v = mb << 20;
        hp_data = hp ? 1 : -1;                     /* 非 2m 一律显式 NOHUGEPAGE */
    } else if (!strcmp(id, "c14")) {               /* 多流计算内核(§4.15) */
        rc.is_loop_read = 0;
        rc.K = 2;
        if (o->sub && !strcmp(o->sub, "matmul")) {
            uint32_t N = o->nops > 0 ? (uint32_t)o->nops : 192;
            rc.kd = kern_get(K_MATMUL, 0); rc.kc = kern_get(K_MATMUL, 1);
            gen_matmul(p, N);                      /* p->n=N,extra_x/y/z,data_size */
            rc.n_ops = 2ULL * N * N * N;           /* mul+add FLOP 归一化 */
            snprintf(param, sizeof(param), "n=%u,matmul", N);
        } else {
            size_t dn = n ? n : (1u << 20);
            rc.kd = kern_get(K_DAXPY, 0); rc.kc = kern_get(K_DAXPY, 1);
            gen_daxpy(p, dn);
            rc.n_ops = 2 * dn;
            snprintf(param, sizeof(param), "n=%zu,daxpy", dn);
        }
    } else {
        fprintf(stderr, "未知用例 %s\n", id);
        return 1;
    }

    uint8_t *base;

    /* ---- c14 专用路径:内核吃"基址流"(addr[0] = X/A,p1 = Y/B,p2 = C) ---- */
    if (!strcmp(id, "c14")) {
        base = mmap_data(p->data_size, 1);
        if (!base) return 1;
        if (setup_map(&rc, base)) return 1;
        p->data = base;
        double *d = (double *)base;
        if (strstr(rc.param, "matmul")) {
            for (uint64_t j = 0; j < 3ULL * (uint64_t)p->n * p->n; j++)
                d[j] = (double)(j % 7) * 0.25 + 0.125;
        } else {
            for (size_t i = 0; i < p->n; i++) d[i] = (double)(i & 1023) * 0.5;
            for (size_t i = 0; i < p->n; i++) d[p->n + i] = 1.0;
        }
        data_warm(base, p->data_size);

        /* 基址装配:addr[0] = X/A 的 VVA 或 VA;p1 = Y/B;p2 = a/C(按模式) */
        uint64_t ax = (uint64_t)(uintptr_t)vva2va_pure(p->extra_x);
        uint64_t ay = (uint64_t)(uintptr_t)vva2va_pure(p->extra_y);
        uint64_t az = (uint64_t)(uintptr_t)vva2va_pure(p->extra_z);
        rc.pat.conv = malloc(8); rc.pat.direct = malloc(8);
        rc.pat.conv[0] = p->extra_x;
        rc.pat.direct[0] = ax;
        rc.p1d = ay; rc.p1c = p->extra_y;
        if (strstr(rc.param, "matmul")) {
            rc.p2d = az; rc.p2c = p->extra_z;
        } else {
            double a = 1.1; memcpy(&rc.p2d, &a, 8); rc.p2c = rc.p2d;
        }
        /* 公平性:基址逐元素核对(单元素) */
        if ((uint64_t)(uintptr_t)vva2va(rc.pat.conv[0]) != rc.pat.direct[0]) {
            fprintf(stderr, "公平性断言失败(c14 基址)\n");
            return 2;
        }
        g_sink = 0;
        int r = run_and_report(&rc, id, o, pmu, csvf);
        free(rc.pat.conv); free(rc.pat.direct);
        munmap(base, p->data_size);
        return r;
    }

    /* ---- 常规用例:列表 + 数据段 ---- */
    size_t cap;
    if (!strcmp(id, "c8")) {
        /* gen_copy 布局:源列表 [0,n),目的列表自 n+1 起 → cap = 2n+2 */
        size_t listn = copy_word ? bytes / 4 : bytes / ((uint64_t)(p->block_words ? p->block_words : 16) * 4);
        cap = 2 * listn + 2;
    } else if (!strcmp(id, "c6")) {
        cap = n + 1;                               /* 仅起点 + derive_direct 需覆盖 n */
    } else {
        cap = n;
    }
    p->conv = calloc(cap, 8);
    p->direct = calloc(cap, 8);
    if (!p->conv || !p->direct) { fprintf(stderr, "oom\n"); return 1; }

    if (!strcmp(id, "c1")) gen_seq(p, bytes);
    else if (!strcmp(id, "c2")) gen_hot(p, n, p->window);
    else if (!strcmp(id, "c3")) gen_randoff(p, n, span_v);
    else if (!strcmp(id, "c4") || !strcmp(id, "c4b")) gen_randidx(p, n);
    else if (!strcmp(id, "c5")) gen_randidx(p, n);
    else if (!strcmp(id, "c6")) gen_chase(p, n);
    else if (!strcmp(id, "c7")) gen_stride(p, n);
    else if (!strcmp(id, "c8")) gen_copy(p, bytes, copy_word);
    else if (!strcmp(id, "c9")) gen_gather(p, n);
    else if (!strcmp(id, "c10")) gen_zipf(p, n, 0.9);
    else if (!strcmp(id, "c11")) gen_randoff(p, n, span_v);
    else if (!strcmp(id, "c12")) gen_randoff(p, n, span_v);

    if (!rc.n_ops) rc.n_ops = p->n;

    /* c8:目的 direct 列表与 conv 列表同布局(derive_direct 双流填充) */
    if (!strcmp(id, "c8")) p->dst_direct = &p->direct[p->n + 1];

    /* c12 需在首次触碰前设置 THP 策略 → 不带 MAP_POPULATE 分配 */
    base = mmap_data(p->data_size, !strcmp(id, "c12") ? 0 : 1);
    if (!base) return 1;
    if (!strcmp(id, "c12")) {
        /* THP 显式开关(§3.3):2m → MADV_HUGEPAGE(触碰时同步大页故障),
         * 其余 → MADV_NOHUGEPAGE,排除 THP 的影响 */
        madvise(base, p->data_size, hp_data > 0 ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
    }
    rc.K = rc.K ? rc.K : 2;
    if (setup_map(&rc, base)) return 1;
    p->data = base;

    if (!strcmp(id, "c8")) {                       /* 源数据填充(目的区由内核写) */
        uint32_t *w = (uint32_t *)base;
        for (uint64_t i = 0; i < bytes / 4; i++) w[i] = (uint32_t)i;
    }
    if (!strcmp(id, "c6")) fill_chain(p, 1);       /* 映射后重填链值(conv 形式) */

    derive_direct(p);
    if (check_fairness(p)) return 2;

    /* c8:p1 = 目的列表指针(按模式给 direct/conv 实例各自的形式) */
    if (!strcmp(id, "c8")) {
        rc.p1d = (uint64_t)(uintptr_t)p->dst_direct;
        rc.p1c = (uint64_t)(uintptr_t)p->dst_conv;
    }

    data_warm(base, p->data_size);
    g_sink = 0;

    int r = run_and_report(&rc, id, o, pmu, csvf);
    free_pat(p);
    munmap(base, p->data_size);
    g_hop_hook = NULL;
    return r;
}

/* ---------------- C13 便捷别名:当前构建变体跑 c2 + c4 扫描 ---------------- */

static int run_c13(struct opts *o, struct pmuset *pmu, FILE *csvf) {
    int rc = 0;
    struct opts o2 = *o;
    rc |= case_run("c2", &o2, pmu, csvf);
    static const long ks[] = { 4096, 65536, 1048576 };
    for (int i = 0; i < 3; i++) {
        o2 = *o; o2.k = ks[i];
        rc |= case_run("c4", &o2, pmu, csvf);
    }
    return rc;
}

/* ============================ CLI ============================ */

static void usage(void) {
    printf(
"vva_bench —— VVA→VA 转换 overhead 测试框架(test.md)\n"
"  --case c0..c14[,c..]   用例(c13 = 当前变体跑 c2+c4 扫描)\n"
"  --k N                  表索引范围(C4/C5/C7/C9/C10)\n"
"  --nops N               内核迭代数覆盖\n"
"  --reps N               重复次数(默认 30,§3.6)\n"
"  --bytes N              流/拷贝字节数(C1/C8)\n"
"  --window N             C2 窗口字节(8..64)\n"
"  --span N               C3/C11/C12 段大小\n"
"  --stride N             C7 idx 步长\n"
"  --win-bytes N          C9 数据窗口\n"
"  --block N              C8 块字节(64/128)\n"
"  --mb N                 C12 数据足迹(MB)\n"
"  --sub X                子模式:c1 w;c6 cold;c8 word|nt;c11 r|rmw|w;c12 2m;\n"
"                         c14 matmul(默认 daxpy)\n"
"  --csv FILE             CSV 追加输出(§5.1 schema)\n"
"  --seed N               RNG 种子\n"
"  --llc-mb N             evict 缓冲大小(默认 96)\n"
"  --flush auto|cl|evict  表 flush 方式(§3.4)\n"
"  --raw-l2m HEX          L2 miss raw config(<0 关;ARM 默认 0x17)\n"
"  --table-hp             表 madvise(MADV_HUGEPAGE)(需 HP=1 构建)\n"
"  --variant NAME         CSV variant 列\n"
"  --arch/--enc/--tag/--base/--isa NAME  校验与构建一致\n"
"  --force                跳过构建一致性校验\n"
"  --no-ins-check         关闭 Δins/op 硬校验\n"
"  --list                 列出用例\n");
}

static void list_cases(void) {
    printf("c0 差分正确性(gate)   c1 顺序流           c2 相近VVA热点(必测)\n"
           "c3 段内随机            c4 随机索引(必测)   c4b 独立页随机索引\n"
           "c5 表全冷              c6 指针追逐(cold子模式)  c7 跨段步长\n"
           "c8 拷贝带宽(word/nt)  c9 gather/scatter   c10 Zipf\n"
           "c11 读写混合(r/rmw/w)c12 TLB交互(2m)     c13 变体矩阵别名\n"
           "c14 计算内核(daxpy/matmul)\n");
}

int main(int argc, char **argv) {
    /* pinned 构建:main 入口最早时刻钉住驻留寄存器(glibc 启动路径结束后,见 src/vva_map.c) */
#if defined(VVA_BASE_PINNED)
    vva_tbl_pin();
#endif

    static struct option lo[] = {
        {"case", 1, 0, 'c'}, {"k", 1, 0, 'k'}, {"nops", 1, 0, 'n'},
        {"reps", 1, 0, 'r'}, {"bytes", 1, 0, 'b'}, {"window", 1, 0, 'w'},
        {"span", 1, 0, 's'}, {"stride", 1, 0, 't'}, {"win-bytes", 1, 0, 'W'},
        {"block", 1, 0, 'B'}, {"mb", 1, 0, 'M'}, {"sub", 1, 0, 'S'},
        {"csv", 1, 0, 'o'}, {"seed", 1, 0, 'e'}, {"llc-mb", 1, 0, 'L'},
        {"flush", 1, 0, 'F'}, {"raw-l2m", 1, 0, 'R'}, {"table-hp", 0, 0, 'H'},
        {"variant", 1, 0, 'V'}, {"arch", 1, 0, 'A'}, {"enc", 1, 0, 'E'},
        {"tag", 1, 0, 'T'}, {"base", 1, 0, 'P'}, {"isa", 1, 0, 'I'},
        {"force", 0, 0, 'f'}, {"no-ins-check", 0, 0, 'x'}, {"list", 0, 0, 'l'},
        {"help", 0, 0, 'h'}, {0, 0, 0, 0}
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "c:k:n:r:b:w:s:t:W:B:M:S:o:e:L:F:R:HV:A:E:T:P:Ifxlh", lo, NULL)) >= 0) {
        switch (ch) {
        case 'c': O.cases = optarg; break;
        case 'k': O.k = atol(optarg); break;
        case 'n': O.nops = atol(optarg); break;
        case 'r': O.reps = atol(optarg); break;
        case 'b': O.bytes = atol(optarg); break;
        case 'w': O.window = atol(optarg); break;
        case 's': O.span = atol(optarg); break;
        case 't': O.stride = atol(optarg); break;
        case 'W': O.win_bytes = atol(optarg); break;
        case 'B': O.block = atol(optarg); break;
        case 'M': O.mb = atol(optarg); break;
        case 'S': O.sub = optarg; break;
        case 'o': O.csv = optarg; break;
        case 'e': O.seed = atoll(optarg); break;
        case 'L': O.llc_mb = atol(optarg); break;
        case 'F': O.flush = optarg; break;
        case 'R': O.raw_l2m = strtol(optarg, NULL, 0); break;
        case 'H': O.table_hp = 1; break;
        case 'V': O.variant = optarg; break;
        case 'A': O.arch = optarg; break;
        case 'E': O.enc = optarg; break;
        case 'T': O.tag = optarg; break;
        case 'P': O.base = optarg; break;
        case 'I': O.isa = optarg; break;
        case 'f': O.force = 1; break;
        case 'x': O.no_ins_check = 1; break;
        case 'l': list_cases(); return 0;
        case 'h': usage(); return 0;
        default: return 1;
        }
    }

    /* 构建一致性校验(§7:C13 结论按编译器/变体分栏,须防张冠李戴) */
    struct { const char *cli, *build, *name; } chk[] = {
        { O.arch, VVA_BUILD_ARCH, "arch" }, { O.enc, VVA_BUILD_ENC, "enc" },
        { O.tag, VVA_BUILD_TAG, "tag" },    { O.base, VVA_BUILD_BASE, "base" },
        { O.isa, VVA_BUILD_ISA, "isa" },
    };
    for (int i = 0; i < 5; i++)
        if (chk[i].cli && strcmp(chk[i].cli, chk[i].build) && !O.force) {
            fprintf(stderr, "--%s=%s 与构建(%s)不一致;--force 可跳过\n",
                    chk[i].name, chk[i].cli, chk[i].build);
            return 1;
        }

    int cl = !strcmp(O.flush, "cl") ||
             (!strcmp(O.flush, "auto"));   /* x86 恒可用;ARM 由运行时 SIGILL 探测回退 */
    cache_ctl_init(O.llc_mb, cl);
    if (O.table_hp) vva_table_enable_hugepage();

    struct pmuset pmu;
    pmu_open(&pmu, O.raw_l2m >= 0, O.raw_l2m);
    if (!pmu.ok)
        fprintf(stderr, "警告:perf 不可用(paranoid/CPU?),回退%s墙钟计时,仅 cycles 有效\n",
                G_ARCH_IS_X86 ? " lfence;rdtsc " : " cntvct_el0 ");

    FILE *csvf = NULL;
    if (O.csv) {
        csvf = fopen(O.csv, "a");
        if (!csvf) { perror("csv"); return 1; }
        fseek(csvf, 0, SEEK_END);
        if (ftell(csvf) == 0)
            fprintf(csvf, "arch,variant,enc,tag,base,isa,w,case,param,rep,"
                          "direct_cyc,conv_cyc,n_ops,direct_ins,conv_ins,"
                          "direct_l1m,conv_l1m,direct_l2m,conv_l2m,"
                          "direct_llcm,conv_llcm,direct_dtlb,conv_dtlb,freq_mhz,"
                          "pmu,mad_cyc_op,ins_expect,ins_ok\n");
    }

    printf("arch=%s variant=%s enc=%s tag=%s base=%s isa=%s W=%d(VVA_HIGH_BITS) cpu=%d%s\n",
           VVA_BUILD_ARCH, O.variant, VVA_BUILD_ENC, VVA_BUILD_TAG, VVA_BUILD_BASE,
           VVA_BUILD_ISA, VVA_BUILD_W, current_cpu(), pmu.scaled ? " PMU=多路复用" : "");

    int rc = 0;
    char *cases = strdup(O.cases);
    for (char *tok = strtok(cases, ","); tok; tok = strtok(NULL, ",")) {
        if (!strcmp(tok, "c13")) { rc |= run_c13(&O, &pmu, csvf); continue; }
        rc |= case_run(tok, &O, &pmu, csvf);
    }
    free(cases);

    if (pmu.ok) pmu_close(&pmu);
    if (csvf) fclose(csvf);
    return rc ? 2 : 0;
}
