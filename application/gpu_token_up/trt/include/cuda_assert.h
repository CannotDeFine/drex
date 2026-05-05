#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstdint>

#include <cuda_runtime.h>

#include "utils.h"

#define CUDART_ASSERT(cmd)          \
    do {                            \
        cudaError_t result = cmd;   \
        if (UNLIKELY(result != cudaSuccess)) { \
            ERRO("cuda runtime error %d", result); \
        }                           \
    } while (0)
