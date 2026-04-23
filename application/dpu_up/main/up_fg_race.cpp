#include <chrono>
#include <cstdio>
#include <fstream>

#include "runtime.h"
#include "utils.h"

#define WARMUP_TASKS 200
#define RACE_TASKS 5000

using namespace dpu_up::utils;

int main(int argc, char **argv)
{
    if (argc < 2) {
        XINFO("usage: %s <out>", argv[0]);
        XERRO("lack arguments, abort...");
    }

    const std::string out(argv[1]);
    const uint64_t warmup_tasks =
        static_cast<uint64_t>(dpu_up::GetEnvInt64OrDefault("DPU_UP_RACE_WARMUP_TASKS", WARMUP_TASKS));
    const uint64_t race_tasks =
        static_cast<uint64_t>(dpu_up::GetEnvInt64OrDefault("DPU_UP_RACE_TASKS", RACE_TASKS));

    ProcessSync psync;
    dpu_up::InitDocaLogging();
    psync.Sync(2, "Fg race init done");

    if (warmup_tasks > 0 && dpu_up::RunForTaskCount(warmup_tasks) == 0) {
        XERRO("fg race warmup failed");
    }

    psync.Sync(4, "Fg race warmup done");
    psync.Sync(6, "Fg race start");
    const auto start = std::chrono::steady_clock::now();
    const uint64_t completed = dpu_up::RunForTaskCount(race_tasks);
    if (completed != race_tasks) XERRO("fg race task count mismatch");
    const auto end = std::chrono::steady_clock::now();

    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double elapsed_s = static_cast<double>(ns) / 1e9;
    const double thpt = elapsed_s <= 0.0 ? 0.0 : static_cast<double>(completed) / elapsed_s;

    std::ofstream file(out);
    file << elapsed_s << " " << thpt << std::endl;
    file.close();
    std::printf("RESULT role=fg-race elapsed=%.6f s throughput=%.2f tasks/s\n", elapsed_s, thpt);
    std::fflush(stdout);

    XINFO("[RACE] Fg finished %lu tasks in %.6f s (%.2f tasks/s)", completed, elapsed_s, thpt);
    dpu_up::ShutdownRuntime();
    return 0;
}
