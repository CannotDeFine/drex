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
    psync.Sync(2, "Bg build start");

    cudaStream_t stream;
    CUDART_ASSERT(cudaStreamCreate(&stream));
    TRTModel model(model_name + ".onnx", model_name + ".engine", batch_size);

    for (size_t i = 0; i < WARMUP_CNT; ++i) {
        model.Infer(stream);
    }

    psync.Sync(4, "Bg warmup done");
    int64_t cnt = 0;
    const int64_t ns = EXEC_TIME(nanoseconds, {
        while (psync.GetCnt() < 5) {
            model.Infer(stream);
            cnt++;
        }
    });
    psync.Sync(7, "Bg done");

    const double thpt = static_cast<double>(cnt) * 1e9 / ns;
    std::ofstream file(out, std::ios::app);
    file << thpt << std::endl;
    file.close();
    INFO("[RESULT] Bg throughput %.2f reqs/s", thpt);

    CUDART_ASSERT(cudaStreamDestroy(stream));
    return 0;
}
