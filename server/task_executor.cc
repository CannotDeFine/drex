#include "task_executor.h"

TaskExecutor::TaskExecutor(const TaskRequest& request) : request_(request) {}
TaskExecutor::~TaskExecutor() = default;