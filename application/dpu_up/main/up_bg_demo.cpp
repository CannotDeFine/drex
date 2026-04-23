#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <thread>

#include "runtime.h"
#include "utils.h"
#include "xsched/doca/hal.h"
#include "xsched/doca/hal/doca_command.h"
#include "xsched/hint.h"
#include "xsched/preempt/hal/hw_queue.h"
#include "xsched/preempt/xqueue/xqueue.h"
#include "xsched/xqueue.h"

#define DEMO_WARMUP_CMDS 200
#define DEMO_MEASURE_SEC 8
#define DEMO_CMD_US 2000
#define DEMO_BACKLOG 1

using namespace dpu_up::utils;
using namespace xsched::doca;
using namespace xsched::preempt;

namespace {

class DemoRunner {
  public:
    DemoRunner()
    {
        fake_dpa_ = reinterpret_cast<doca_dpa *>(&token_);
        XResult res = DocaQueueCreate(&hwq_, fake_dpa_);
        if (res != kXSchedSuccess) XERRO("DocaQueueCreate failed: %d", res);

        res = XQueueCreate(&xq_handle_, hwq_, kPreemptLevelBlock, kQueueCreateFlagNone);
        if (res != kXSchedSuccess) XERRO("XQueueCreate failed: %d", res);

        const int64_t threshold = dpu_up::GetEnvInt64OrDefault("XSCHED_AUTO_XQUEUE_THRESHOLD", 1);
        const int64_t batch_size = dpu_up::GetEnvInt64OrDefault("XSCHED_AUTO_XQUEUE_BATCH_SIZE", 1);
        res = XQueueSetLaunchConfig(xq_handle_, threshold, batch_size);
        if (res != kXSchedSuccess) XERRO("XQueueSetLaunchConfig failed: %d", res);

        const int64_t util = dpu_up::GetEnvInt64OrDefault("XSCHED_AUTO_XQUEUE_UTILIZATION", -1);
        if (util >= 0) {
            res = XHintUtilization(xq_handle_, util);
            if (res != kXSchedSuccess) XERRO("XHintUtilization failed: %d", res);
        }

        const int64_t timeslice_us = dpu_up::GetEnvInt64OrDefault("XSCHED_AUTO_XQUEUE_TIMESLICE", -1);
        if (timeslice_us > 0) {
            res = XHintTimeslice(timeslice_us);
            if (res != kXSchedSuccess) XERRO("XHintTimeslice failed: %d", res);
        }

        xq_ = XQueueManager::Get(xq_handle_);
        if (xq_ == nullptr) XERRO("XQueueManager::Get returned null");
    }

    ~DemoRunner()
    {
        if (xq_handle_ != 0) (void)XQueueDestroy(xq_handle_);
        if (hwq_ != 0) (void)HwQueueDestroy(hwq_);
    }

    void Warmup(uint64_t count, uint64_t cmd_us)
    {
        if (count == 0) return;
        auto counter = std::make_shared<std::atomic<uint64_t>>(0);
        SubmitFixedCount(count, cmd_us, counter);
        xq_->WaitAll();
    }

    uint64_t RunForDuration(uint64_t duration_ns, uint64_t cmd_us, uint64_t backlog)
    {
        auto counter = std::make_shared<std::atomic<uint64_t>>(0);
        uint64_t submitted = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(duration_ns);

        while (std::chrono::steady_clock::now() < deadline) {
            uint64_t completed = counter->load(std::memory_order_relaxed);
            while (submitted - completed < backlog && std::chrono::steady_clock::now() < deadline) {
                xq_->Submit(MakeCommand(cmd_us, counter));
                ++submitted;
                completed = counter->load(std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }

        const uint64_t completed_before_drain = counter->load(std::memory_order_relaxed);
        xq_->WaitAll();
        return completed_before_drain;
    }

  private:
    std::shared_ptr<DocaCommand> MakeCommand(uint64_t cmd_us,
                                             const std::shared_ptr<std::atomic<uint64_t>> &counter)
    {
        auto cmd = std::make_shared<DocaCommand>(fake_dpa_);
        cmd->SetLaunchCallback([cmd_us, counter](doca_dpa *, DocaCommand &) -> doca_error_t {
            std::this_thread::sleep_for(std::chrono::microseconds(cmd_us));
            counter->fetch_add(1, std::memory_order_relaxed);
            return DOCA_SUCCESS;
        });
        return cmd;
    }

    void SubmitFixedCount(uint64_t count,
                          uint64_t cmd_us,
                          const std::shared_ptr<std::atomic<uint64_t>> &counter)
    {
        for (uint64_t i = 0; i < count; ++i) {
            xq_->Submit(MakeCommand(cmd_us, counter));
        }
    }

    uintptr_t token_ = 0x1;
    doca_dpa *fake_dpa_ = nullptr;
    HwQueueHandle hwq_ = 0;
    XQueueHandle xq_handle_ = 0;
    std::shared_ptr<XQueue> xq_;
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        XINFO("usage: %s <out>", argv[0]);
        XERRO("lack arguments, abort...");
    }

    const std::string out(argv[1]);
    const uint64_t warmup_cmds =
        static_cast<uint64_t>(dpu_up::GetEnvInt64OrDefault("DPU_UP_DEMO_WARMUP_CMDS", DEMO_WARMUP_CMDS));
    const uint64_t measure_ns = static_cast<uint64_t>(
        dpu_up::GetEnvInt64OrDefault("DPU_UP_DEMO_MEASURE_SEC", DEMO_MEASURE_SEC)) * 1000ULL * 1000ULL * 1000ULL;
    const uint64_t cmd_us =
        static_cast<uint64_t>(dpu_up::GetEnvInt64OrDefault("DPU_UP_DEMO_CMD_US", DEMO_CMD_US));
    const uint64_t backlog =
        static_cast<uint64_t>(dpu_up::GetEnvInt64OrDefault("DPU_UP_DEMO_BACKLOG", DEMO_BACKLOG));

    ProcessSync psync;
    psync.Sync(2, "Bg demo init done");

    DemoRunner runner;
    runner.Warmup(warmup_cmds, cmd_us);

    psync.Sync(4, "Bg demo warmup done");
    const auto start = std::chrono::steady_clock::now();
    const uint64_t completed = runner.RunForDuration(measure_ns, cmd_us, backlog);
    const auto end = std::chrono::steady_clock::now();

    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double elapsed_s = static_cast<double>(ns) / 1e9;
    const double rate = elapsed_s <= 0.0 ? 0.0 : static_cast<double>(completed) / elapsed_s;

    std::ofstream file(out);
    file << completed << " " << elapsed_s << " " << rate << std::endl;
    file.close();
    std::printf("RESULT role=bg-demo completed=%lu elapsed=%.6f s rate=%.2f cmds/s\n", completed, elapsed_s, rate);
    std::fflush(stdout);

    XINFO("[DEMO] Bg completed %lu cmds in %.6f s (%.2f cmds/s)", completed, elapsed_s, rate);
    return 0;
}
