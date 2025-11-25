#ifndef TASK_FACTORY_H
#define TASK_FACTORY_H

#include <memory>

#include "asic_task.h"
#include "cpu_task.h"
#include "dpu_task.h"
#include "fpga_task.h"
#include "gpu_task.h"
#include "npu_task.h"
#include "task_executor.h"

class TaskFactory {
  public:
    static std::unique_ptr<TaskExecutor> Create(const TaskRequest &request) {
        switch (request.device_type()) {
        case remote_service::kOFA:
            return std::make_unique<OfaExecutor>(request);
        case remote_service::kCPU:
            return std::make_unique<CpuExecutor>(request);
        case remote_service::kGPU:
            return std::make_unique<GpuExecutor>(request);
        case remote_service::kDPU:
            return std::make_unique<DpuExecutor>(request);
        case remote_service::kFPGA:
            return std::make_unique<FpgaExecutor>(request);
        case remote_service::kNPU:
            return std::make_unique<NpuExecutor>(request);
        // TODO: Add new device types here.
        default:
            return nullptr;
        }
    }
};

#endif
