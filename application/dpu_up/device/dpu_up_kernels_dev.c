#include <doca_dpa_dev.h>

__dpa_global__ void xsched_compute_kernel(uint64_t iters, uint64_t seed)
{
    volatile uint64_t acc = seed + 1;

    for (uint64_t i = 0; i < iters; ++i) {
        acc = acc * 1664525ULL + 1013904223ULL + i;
    }

    if (acc == 0) {
        DOCA_DPA_DEV_LOG_INFO("unexpected zero accumulator\n");
    }
}
