#include <chrono>
#include <cstdio>
#include <fstream>

#include "runtime.h"
#include "utils.h"

#define WARMUP_CNT 20
#define MEASURE_SEC 15
#define WARMUP_MS 50

using namespace dpu_up::utils;

int main(int argc, char **argv)
{
    if (argc < 2) {
        XINFO("usage: %s <out>", argv[0]);
        XERRO("lack arguments, abort...");
    }

    const std::string out(argv[1]);
    const int64_t warmup_cnt = dpu_up::GetEnvInt64OrDefault("DPU_UP_WARMUP_CNT", WARMUP_CNT);
    const uint64_t warmup_ns = static_cast<uint64_t>(
        dpu_up::GetEnvInt64OrDefault("DPU_UP_WARMUP_MS", WARMUP_MS)) * 1000ULL * 1000ULL;
    const uint64_t measure_ns = static_cast<uint64_t>(
        dpu_up::GetEnvInt64OrDefault("DPU_UP_MEASURE_SEC", MEASURE_SEC)) * 1000ULL * 1000ULL * 1000ULL;
    ProcessSync psync;
    dpu_up::InitDocaLogging();
    psync.Sync(2, "Fg build done");

    for (int64_t i = 0; i < warmup_cnt; ++i) {
        if (dpu_up::RunForDurationNs(warmup_ns) == 0) XERRO("fg warmup window failed");
    }

    psync.Sync(4, "Fg warmup done");
    auto start = std::chrono::steady_clock::now();
    const uint64_t completed = dpu_up::RunForDurationNs(measure_ns);
    if (completed == 0) XERRO("fg benchmark window failed");
    auto end = std::chrono::steady_clock::now();
    psync.Sync(5, "Fg benchmark done");

    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double thpt = (completed == 0 || ns == 0) ? 0.0 : static_cast<double>(completed) * 1e9 / static_cast<double>(ns);
    std::ofstream file(out);
    file << thpt << std::endl;
    file.close();
    std::printf("RESULT role=fg throughput=%.2f tasks/s\n", thpt);
    std::fflush(stdout);
    XINFO("[RESULT] Fg throughput %.2f tasks/s", thpt);
    psync.Sync(7, "Fg written");
    dpu_up::ShutdownRuntime();
    return 0;
}
