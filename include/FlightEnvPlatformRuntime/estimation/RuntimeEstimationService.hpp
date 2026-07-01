#pragma once

/**
 * @file RuntimeEstimationService.hpp
 * @brief 系统级滤波/融合服务门面。
 *
 * 大概：这是 RuntimeEstimationService 的平台估计入口。
 * 具体：读取编译后的 estimation_plan，消费观测帧，调度样本批次，显式发布后验 commit/checkpoint/evidence。
 * 被谁使用：NativeWorkflowRunner 在 estimation_system workflow 中调用。
 * 使用谁：RuntimeObservationInbox、EstimatorMethodRegistry、SampleSetStore、RuntimeEvidenceWriter。
 */

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime::estimation {

struct RuntimeEstimationRequest {
  std::filesystem::path run_dir;
  std::filesystem::path compiled_workflow_dir;
  std::string run_id;
  std::string workflow_id;
  std::string object_id;
  std::string branch_id;
  std::string timeline_id;
  nlohmann::json estimation_plan = nlohmann::json::object();
  nlohmann::json scheduler_plan = nlohmann::json::object();
  nlohmann::json workflow_snapshot = nlohmann::json::object();
  nlohmann::json external_observations = nlohmann::json::array();
  int max_frames = 0;
};

struct RuntimeEstimationResult {
  int exit_code = 0;
  int frame_count = 0;
  int failed_frame_count = 0;
  nlohmann::json summary = nlohmann::json::object();
};

class RuntimeEstimationService {
 public:
  RuntimeEstimationResult runSerial(const RuntimeEstimationRequest& request) const;
  RuntimeEstimationResult runScheduled(const RuntimeEstimationRequest& request) const;
};

}  // namespace FlightEnvPlatformRuntime::estimation
