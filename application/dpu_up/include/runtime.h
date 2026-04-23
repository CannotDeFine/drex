#pragma once

#include <stdint.h>

namespace dpu_up
{

void InitDocaLogging();
uint64_t RunForDurationNs(uint64_t duration_ns);
uint64_t RunForTaskCount(uint64_t task_count);
void ShutdownRuntime();
int64_t GetEnvInt64OrDefault(const char *name, int64_t default_value);

} // namespace dpu_up
