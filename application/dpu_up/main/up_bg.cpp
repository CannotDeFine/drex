#include <chrono>
#include <fstream>

#include "runtime.h"
#include "utils.h"

#define WARMUP_CNT 20

using namespace dpu_up::utils;

int main(int argc, char **argv)
{
    if (argc < 2) {
        XINFO("usage: %s <out>", argv[0]);
        XERRO("lack arguments, abort...");
    }

    const std::string out(argv[1]);
    const int64_t warmup_cnt = WARMUP_CNT;

    ProcessSync psync;
    dpu_up::InitDocaLogging();
    psync.Sync(2, "Bg build start");

    for (int64_t i = 0; i < warmup_cnt; ++i) {
        if (dpu_up::RunForDurationNs(50ULL * 1000ULL * 1000ULL) == 0) XERRO("bg warmup window failed");
    }

    psync.Sync(4, "Bg warmup done");
    auto start = std::chrono::steady_clock::now();
    const uint64_t completed = dpu_up::RunForDurationNs(8ULL * 1000ULL * 1000ULL * 1000ULL);
    if (completed == 0) XERRO("bg benchmark window failed");
    while (psync.GetCnt() < 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto end = std::chrono::steady_clock::now();

    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double thpt = (completed == 0 || ns == 0) ? 0.0 : static_cast<double>(completed) * 1e9 / static_cast<double>(ns);
    std::ofstream file(out, std::ios::app);
    file << thpt << std::endl;
    file.close();
    psync.Sync(7, "Bg done");
    XINFO("[RESULT] Bg throughput %.2f tasks/s", thpt);
    dpu_up::ShutdownRuntime();
    return 0;
}
