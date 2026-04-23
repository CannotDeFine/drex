/* DPA flow demo: device kernels. */

#include <doca_dpa_dev.h>

__dpa_global__ void hello_world(uint64_t iters, uint64_t task_id, uint64_t verbose)
{
	(void)verbose;
	volatile uint64_t a0 = 0x9E3779B97F4A7C15ULL ^ task_id;
	volatile uint64_t a1 = 0xBF58476D1CE4E5B9ULL + (task_id << 1);
	volatile uint64_t a2 = 0x94D049BB133111EBULL ^ (task_id << 2);
	volatile uint64_t a3 = 0xD6E8FEB86659FD93ULL + (task_id << 3);

	for (uint64_t i = 0; i < iters; i++) {
		uint64_t x = i + (uint64_t)task_id;
		a0 = (a0 ^ (x + (a1 << 1))) * 0x9E3779B185EBCA87ULL;
		a1 = (a1 + (a0 ^ (a2 >> 3)) + 0x165667B19E3779F9ULL);
		a2 = (a2 ^ (a1 + (a3 << 2))) * 0xC2B2AE3D27D4EB4FULL;
		a3 = (a3 + (a2 ^ (a0 >> 5)) + 0x27D4EB2F165667C5ULL);
	}

	volatile uint64_t acc = a0 ^ a1 ^ a2 ^ a3;
	(void)acc;
}
