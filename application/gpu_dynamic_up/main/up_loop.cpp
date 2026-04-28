#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <string>

#include <cuda_runtime.h>

#include "cuda_assert.h"
#include "model.h"
#include "utils.h"

using namespace gpu_up::utils;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void HandleSignal(int) {
    g_stop = 1;
}

long ParsePositiveLong(const char *value, long fallback) {
    if (value == nullptr) {
        return fallback;
    }
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0) {
        return fallback;
    }
    return parsed;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 6) {
        INFO("usage: %s <model prefix> <batch size> <out> <duration_sec> <window_sec>", argv[0]);
        ERRO("lack arguments, abort...");
    }

    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGINT, HandleSignal);

    const std::string model_name(argv[1]);
    const int batch_size = static_cast<int>(ParsePositiveLong(argv[2], 1));
    const std::string out(argv[3]);
    const long duration_sec = ParsePositiveLong(argv[4], 120);
    const long window_sec = ParsePositiveLong(argv[5], 5);
    const long warmup_count = ParsePositiveLong(std::getenv("GPU_DYNAMIC_UP_WARMUP"), 20);

    INFO("Loop benchmark initializing: batch=%d duration=%lds window=%lds warmup=%ld", batch_size, duration_sec, window_sec, warmup_count);

    cudaStream_t stream;
    CUDART_ASSERT(cudaStreamCreate(&stream));
    INFO("Loading TensorRT model: %s", model_name.c_str());
    TRTModel model(model_name + ".onnx", model_name + ".engine", batch_size);

    ProcessSync psync;
    psync.Sync(2, "Model build done");

    INFO("Warmup started");
    for (long i = 0; i < warmup_count; ++i) {
        model.Infer(stream);
    }
    CUDART_ASSERT(cudaStreamSynchronize(stream));
    INFO("Warmup finished");
    psync.Sync(4, "Warmup done");

    std::ofstream file(out, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        ERRO("failed to open output file: %s", out.c_str());
    }
    file << "window,elapsed_sec,iterations,throughput" << std::endl;

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto window_start = start;
    int window_idx = 0;

    INFO("Loop benchmark started: duration=%lds window=%lds", duration_sec, window_sec);
    while (!g_stop) {
        const auto now = clock::now();
        const auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (elapsed_sec >= duration_sec) {
            break;
        }

        int64_t count = 0;
        window_start = clock::now();
        while (!g_stop) {
            model.Infer(stream);
            ++count;
            const auto cur = clock::now();
            const auto win_elapsed = std::chrono::duration_cast<std::chrono::seconds>(cur - window_start).count();
            const auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(cur - start).count();
            if (win_elapsed >= window_sec || total_elapsed >= duration_sec) {
                break;
            }
        }
        CUDART_ASSERT(cudaStreamSynchronize(stream));

        const auto window_end = clock::now();
        const auto window_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(window_end - window_start).count();
        const auto total_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(window_end - start).count();
        const double throughput = window_ns > 0 ? static_cast<double>(count) * 1e9 / static_cast<double>(window_ns) : 0.0;

        ++window_idx;
        file << window_idx << ',' << (static_cast<double>(total_elapsed_ms) / 1000.0) << ',' << count << ',' << throughput << std::endl;
        file.flush();
        INFO("[RESULT] window=%d elapsed=%.1fs throughput %.2f reqs/s", window_idx, static_cast<double>(total_elapsed_ms) / 1000.0, throughput);
    }

    INFO("Loop benchmark finished");
    CUDART_ASSERT(cudaStreamDestroy(stream));
    return 0;
}
