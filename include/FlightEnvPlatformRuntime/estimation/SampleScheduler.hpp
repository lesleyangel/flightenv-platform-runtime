#pragma once

/**
 * @file SampleScheduler.hpp
 * @brief 系统级估计服务的样本批次调度器。
 *
 * 大概：把一个观测帧里的粒子、sigma point 或 ensemble member 拆成 sample batch。
 * 具体：每个批次都会生成 sample_batch 事件、typed buffer 引用，并进入 RuntimeReadyQueueExecutor 准入检查。
 * 被谁使用：RuntimeEstimationService。
 * 使用谁：RuntimeReadyQueueExecutor、RuntimeTypedBufferStore 和 runtime event 类型。
 */

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime::estimation {

struct RuntimeSampleSchedulingRequest {
  std::filesystem::path run_dir;
  nlohmann::json estimation_system = nlohmann::json::object();
  nlohmann::json scheduler_plan = nlohmann::json::object();
  std::string stage_id;
  std::string method_kind;
  std::string sample_kind;
  std::string branch_id;
  std::string timeline_id;
  int frame_index = 0;
  double sample_time_s = 0.0;
  int sample_count = 0;
  int state_dim = 0;
};

class SampleScheduler {
 public:
  nlohmann::json scheduleFrame(const RuntimeSampleSchedulingRequest& request) const;
};

}  // namespace FlightEnvPlatformRuntime::estimation
