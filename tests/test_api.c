/*
 * test_api.c — vamap API / 快路径单元冒烟测试。
 * 覆盖：init/info、vva_alloc/free、create/destroy、vva2va 等价与 strict
 * 掩码、vload/vstore 往返、慢路径首触、pin、alias 负例、stats、参数校验。
 * 返回 0 = 全部通过。
 */
#include "vamap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CK(cond)                                                            \
    do {                                                                    \
        if (!(cond)) {                                                      \
            g_fail++;                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                   \
    } while (0)

int main(void)
{
    vamap_cfg c;
    memset(&c, 0, sizeof(c));
    c.lock_stripes = 1024;
    c.alias_pool_size = 0;
    c.create_latency_ns = 0;

    CK(vamap_init(&c) == 0);
    vamap_info_t info = vamap_get_info();
    CK(info.region_count > 1);
    CK(info.region_limit >= 1 && info.region_limit < info.region_count);
    CK((vamap_arena & 0xFFFFFFFFULL) == 0);         /* 4GiB 对齐 */
    CK(vamap_table != NULL);
    printf("info: regions=%u limit=%u table=%s arena=0x%llx\n",
           info.region_count, info.region_limit, info.table_backing,
           (unsigned long long)info.arena_base);

    /* vva 区间分配 */
    vva_t b = vamap_vva_alloc(8);
    CK(b != (vva_t)-1);
    CK((b & 0xFFFFFFFFULL) == 0);                   /* 基址对齐 idx 边界 */
    vamap_vva_free(b, 8);

    /* create + vva2va 等价（strict：tag 位忽略） */
    CK(vamap_map_create(5) == 0);
    uint64_t e = vamap_table[5];
    CK(e != 0);
    CK(((e - vamap_arena) & 0xFFFFFFFFULL) == 0);
    void *p1 = vva2va((uint64_t)5 << 32);
    CK((uint64_t)(uintptr_t)p1 == (e | 0));
    void *p2 = vva2va(((uint64_t)5 << 32) | 0x80000001ULL);   /* bit31=1 + tag */
    CK((uint64_t)(uintptr_t)p2 == (e | 0x80000001ULL));
    void *p3 = vva2va(((uint64_t)0xDBE << 52) | ((uint64_t)5 << 32) | 4096);
    CK((uint64_t)(uintptr_t)p3 == (e | 4096));      /* 随机 tag（12 位）不影响结果 */

    /* vload/vstore 往返 + 直读一致 */
    vstore64(((uint64_t)5 << 32) | 64, 0x1234567890ABCDEFULL);
    CK(vload64(((uint64_t)5 << 32) | 64) == 0x1234567890ABCDEFULL);
    CK(*(volatile uint64_t *)(uintptr_t)(e | 64) == 0x1234567890ABCDEFULL);
    vstore32(((uint64_t)5 << 32) | 128, 0xDEADBEEFu);
    CK(vload32(((uint64_t)5 << 32) | 128) == 0xDEADBEEFu);
    CK(*(volatile uint32_t *)(uintptr_t)(e | 128) == 0xDEADBEEFu);

    /* 慢路径首触创建（latency 0） */
    vamap_reset_stats();
    CK(vamap_table[7777] == 0);
    CK(vload64((uint64_t)7777 << 32) == 0);         /* 无效 → 创建 → 读零页 */
    CK(vamap_table[7777] != 0);
    vamap_stats st = vamap_get_stats();
    CK(st.creates == 1);
    CK(st.slow_calls == 1);

    /* pin：返回当前窗口基址 */
    void *pinp = vamap_pin((uint64_t)5 << 32);
    CK(pinp != NULL && (uint64_t)(uintptr_t)pinp == e);
    void *pinp2 = vamap_pin((uint64_t)7778 << 32);  /* 无效 → 先创建 */
    CK(pinp2 != NULL && vamap_table[7778] != 0);
    CK((uint64_t)(uintptr_t)pinp2 == vamap_table[7778]);

    /* destroy 与 region 复用（LIFO 空闲链） */
    CK(vamap_map_destroy(5) == 0);
    CK(vamap_table[5] == 0);
    CK(vamap_map_create(9) == 0);
    CK(vamap_table[9] == e);
    vamap_map_destroy(9);

    /* alias 负例 */
    CK(vamap_map_alias(11, 0) != 0);
    CK(vamap_map_alias(11, 0xF0000000u) != 0);

    /* stats 清零 */
    vamap_reset_stats();
    st = vamap_get_stats();
    CK(st.creates == 0 && st.destroys == 0 && st.slow_calls == 0);
    CK(st.spin_ns == 0 && st.slow_ns_total == 0);

    /* 非法参数 */
    CK(vamap_map_create(1u << VAMAP_IDX_BITS) != 0);
    CK(vamap_map_destroy(1u << VAMAP_IDX_BITS) != 0);
    CK(vamap_map_alias(1u << VAMAP_IDX_BITS, 1) != 0);
    CK(vamap_vva_alloc(0) == (vva_t)-1);
    CK(vamap_vva_alloc((1u << VAMAP_IDX_BITS) + 1) == (vva_t)-1);

    /* tsc 标定 */
    double hz = vamap_tsc_hz();
    CK(hz > 1e6 && hz < 1e10);
    printf("tsc: %.3f MHz\n", hz / 1e6);

    vamap_shutdown();

    printf("%s: %s (%d failures)\n", "test_api", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
