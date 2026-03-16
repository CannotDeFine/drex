#include <fstream>
#include <iostream>

#include <cuda_runtime.h>

#include "cuda_assert.h"
#include "model.h"
#include "utils.h"

#define WARMUP_CNT 300
#define TEST_CNT 1000

using namespace gpu_up::utils;

int main(int argc, char **argv) {
    if (argc < 4) {
        INFO("usage: %s <model prefix> <batch size> <out>", argv[0]);
        ERRO("lack arguments, abort...");
    }

    const std::string model_name(argv[1]);
    const int batch_size = atoi(argv[2]);
    const std::string out(argv[3]);

    ProcessSync psync;
    cudaStream_t stream;
    CUDART_ASSERT(cudaStreamCreate(&stream));
    TRTModel model(model_name + ".onnx", model_name + ".engine", batch_size);
    psync.Sync(2, "Fg build done");

    for (size_t i = 0; i < WARMUP_CNT; ++i) {
        model.Infer(stream);
    }

    psync.Sync(4, "Fg warmup done");
    const int64_t ns = EXEC_TIME(nanoseconds, {
        for (int64_t i = 0; i < TEST_CNT; ++i) {
            model.Infer(stream);
        }
    });
    psync.Sync(5, "Fg benchmark done");

    const double thpt = static_cast<double>(TEST_CNT) * 1e9 / ns;
    std::ofstream file(out);
    file << thpt << std::endl;
    file.close();
    INFO("[RESULT] Fg throughput %.2f reqs/s", thpt);
    psync.Sync(7, "Fg written");

    CUDART_ASSERT(cudaStreamDestroy(stream));
    return 0;
}
