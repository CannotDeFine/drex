#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <random>
#include <vector>
#include <algorithm>

#define VECTOR_SIZE (1 << 18) // 33,554,432 floats (~128MiB per vector)
#define N 200                 // Number of vector additions per task
#define M 200               // Number of tasks

static std::vector<float> A, B, C;

static void prepareTask() {
    A.resize(VECTOR_SIZE);
    B.resize(VECTOR_SIZE);
    C.resize(VECTOR_SIZE);

    // Match your original behavior: use rand() / RAND_MAX
    // (Optional: srand(...) if you want different values each run)
    for (size_t i = 0; i < VECTOR_SIZE; ++i) {
        A[i] = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
        B[i] = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
    }
}

static inline void vectorAddCPU(const float* a, const float* b, float* c, size_t n) {
    // Simple loop; compiler will often auto-vectorize with -O3
    for (size_t i = 0; i < n; ++i) {
        c[i] = a[i] + b[i];
    }
}

static void runTask(int /*taskId*/) {
    for (int i = 0; i < N; ++i) {
        vectorAddCPU(A.data(), B.data(), C.data(), VECTOR_SIZE);
    }

    // Prevent the compiler from optimizing away the work in some extreme cases
    // (very lightweight use: touch one element)
    volatile float sink = C[VECTOR_SIZE / 2];
    (void)sink;
}

static void cleanupTask() {
    std::vector<float>().swap(A);
    std::vector<float>().swap(B);
    std::vector<float>().swap(C);
}

int main() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(30, 50);

    prepareTask();

    for (int i = 0; i < M; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        runTask(i);
        auto end = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::printf("Task %d completed in %lld ms\n", i, static_cast<long long>(duration.count()));

        if (i < M - 1) {
            int sleepTime = dis(gen);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
        }
    }

    cleanupTask();
    return 0;
}
