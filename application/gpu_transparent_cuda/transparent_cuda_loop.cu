#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <cuda_runtime.h>

namespace {

constexpr int kDefaultVectorSize = 1 << 25;
constexpr int kDefaultKernelsPerTask = 100;

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

void CheckCuda(cudaError_t err, const char *expr, const char *file, int line) {
    if (err == cudaSuccess) {
        return;
    }
    std::fprintf(stderr, "[ERROR] CUDA call failed: %s at %s:%d: %s\n", expr, file, line, cudaGetErrorString(err));
    std::fflush(stderr);
    std::abort();
}

#define CUDA_CHECK(expr) CheckCuda((expr), #expr, __FILE__, __LINE__)

__global__ void VecAdd(const float *a, const float *b, float *c, int n) {
    const int i = blockDim.x * blockIdx.x + threadIdx.x;
    if (i < n) {
        c[i] = a[i] + b[i];
    }
}

class CudaWorkload {
  public:
    CudaWorkload(int vector_size, int kernels_per_task)
        : vector_size_(vector_size), kernels_per_task_(kernels_per_task) {
        const size_t bytes = static_cast<size_t>(vector_size_) * sizeof(float);
        host_a_ = static_cast<float *>(std::malloc(bytes));
        host_b_ = static_cast<float *>(std::malloc(bytes));
        if (host_a_ == nullptr || host_b_ == nullptr) {
            std::fprintf(stderr, "[ERROR] failed to allocate host buffers\n");
            std::abort();
        }
        for (int i = 0; i < vector_size_; ++i) {
            host_a_[i] = static_cast<float>(i % 1024);
            host_b_[i] = static_cast<float>((i * 3) % 1024);
        }

        CUDA_CHECK(cudaMalloc(&dev_a_, bytes));
        CUDA_CHECK(cudaMalloc(&dev_b_, bytes));
        CUDA_CHECK(cudaMalloc(&dev_c_, bytes));
        CUDA_CHECK(cudaMemcpy(dev_a_, host_a_, bytes, cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(dev_b_, host_b_, bytes, cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaStreamCreate(&stream_));
    }

    ~CudaWorkload() {
        if (stream_ != nullptr) {
            cudaStreamDestroy(stream_);
        }
        cudaFree(dev_a_);
        cudaFree(dev_b_);
        cudaFree(dev_c_);
        std::free(host_a_);
        std::free(host_b_);
    }

    void RunTask() {
        constexpr int block_size = 256;
        const int grid_size = (vector_size_ + block_size - 1) / block_size;
        for (int i = 0; i < kernels_per_task_; ++i) {
            VecAdd<<<grid_size, block_size, 0, stream_>>>(dev_a_, dev_b_, dev_c_, vector_size_);
        }
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaStreamSynchronize(stream_));
    }

  private:
    int vector_size_;
    int kernels_per_task_;
    float *host_a_ = nullptr;
    float *host_b_ = nullptr;
    float *dev_a_ = nullptr;
    float *dev_b_ = nullptr;
    float *dev_c_ = nullptr;
    cudaStream_t stream_ = nullptr;
};

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <out.csv> <duration_sec> <window_sec>\n", argv[0]);
        return 2;
    }

    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGINT, HandleSignal);

    const std::string out(argv[1]);
    const long duration_sec = ParsePositiveLong(argv[2], 120);
    const long window_sec = ParsePositiveLong(argv[3], 5);
    const int vector_size = static_cast<int>(ParsePositiveLong(std::getenv("GPU_TRANSPARENT_CUDA_VECTOR_SIZE"), kDefaultVectorSize));
    const int kernels_per_task = static_cast<int>(ParsePositiveLong(std::getenv("GPU_TRANSPARENT_CUDA_KERNELS_PER_TASK"), kDefaultKernelsPerTask));
    const long warmup_tasks = ParsePositiveLong(std::getenv("GPU_TRANSPARENT_CUDA_WARMUP_TASKS"), 5);

    std::fprintf(stderr,
                 "[INFO] Transparent CUDA benchmark initializing: duration=%lds window=%lds vector_size=%d kernels_per_task=%d warmup=%ld\n",
                 duration_sec,
                 window_sec,
                 vector_size,
                 kernels_per_task,
                 warmup_tasks);

    CudaWorkload workload(vector_size, kernels_per_task);
    for (long i = 0; i < warmup_tasks && !g_stop; ++i) {
        workload.RunTask();
    }
    std::fprintf(stderr, "[INFO] Warmup finished\n");

    std::ofstream file(out, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        std::fprintf(stderr, "[ERROR] failed to open output file: %s\n", out.c_str());
        return 1;
    }
    file << "window,elapsed_sec,tasks,throughput_tasks_per_sec" << std::endl;

    using clock = std::chrono::steady_clock;
    const long window_count = (duration_sec + window_sec - 1) / window_sec;
    const auto benchmark_start = clock::now();

    std::fprintf(stderr, "[INFO] Benchmark started: duration=%lds window=%lds windows=%ld\n", duration_sec, window_sec, window_count);
    for (long window_idx = 1; window_idx <= window_count && !g_stop; ++window_idx) {
        const long current_window_sec = std::min(window_sec, duration_sec - (window_idx - 1) * window_sec);
        long tasks = 0;
        const auto window_start = clock::now();
        while (!g_stop) {
            workload.RunTask();
            ++tasks;
            const auto now = clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - window_start).count();
            if (elapsed >= current_window_sec) {
                break;
            }
        }

        const auto window_end = clock::now();
        const auto window_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(window_end - window_start).count();
        const auto total_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(window_end - benchmark_start).count();
        const double throughput = window_ns > 0 ? static_cast<double>(tasks) * 1e9 / static_cast<double>(window_ns) : 0.0;

        file << window_idx << ',' << (static_cast<double>(total_elapsed_ms) / 1000.0) << ',' << tasks << ',' << throughput << std::endl;
        file.flush();
        std::fprintf(stderr,
                     "[INFO] [RESULT] window=%ld elapsed=%.1fs throughput %.2f tasks/s\n",
                     window_idx,
                     static_cast<double>(total_elapsed_ms) / 1000.0,
                     throughput);
    }

    std::fprintf(stderr, "[INFO] Benchmark finished\n");
    return 0;
}

