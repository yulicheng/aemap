/* bench/kernels.c —— 内核双实例实例化(test.md §3.8)
 * 同一份 kernel_defs.h 以 CONV=0/1 各展开一次:A2P 一层之差,其余逐字相同。
 */
#include "vva.h"
#include "kernels.h"
#include <string.h>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

volatile uint64_t g_sink;
void (*volatile g_hop_hook)(void);

#define CONV 0
#include "kernel_defs.h"
#undef CONV
#define CONV 1
#include "kernel_defs.h"
#undef CONV

static const kern_fn ktab_d[K_N] = {
    k_read_d, k_write_d, k_rmw_d, k_copy_blk_d, k_copy_word_d, k_copy_nt_d,
    k_chase_d, k_daxpy_d, k_scale_d, k_add_d, k_matmul_d
};
static const kern_fn ktab_c[K_N] = {
    k_read_c, k_write_c, k_rmw_c, k_copy_blk_c, k_copy_word_c, k_copy_nt_c,
    k_chase_c, k_daxpy_c, k_scale_c, k_add_c, k_matmul_c
};

kern_fn kern_get(enum kern_id id, int conv) {
    return conv ? ktab_c[id] : ktab_d[id];
}
