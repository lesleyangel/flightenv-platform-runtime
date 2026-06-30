#pragma once

/**
 * @file SampleSetStore.hpp
 * @brief 保存系统级估计的后验状态和 checkpoint 摘要。
 *
 * 大概：Phase 2 先提供最小内存后验记录，为 evidence、回放和后续分支 seed 提供稳定格式。
 * 具体：记录每个观测帧对应的后验向量、不确定性摘要和 checkpoint id。
 * 被谁使用：RuntimeEstimationService 和估计方法实现。
 * 使用谁：nlohmann::json 和标准容器。
 */

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace FlightEnvPlatformRuntime::estimation {

struct PosteriorFrame {
  int frame_index = 0;
  double sample_time_s = 0.0;
  std::string checkpoint_id;
  std::vector<std::string> value_labels;
  std::vector<double> state_mean;
  std::vector<double> covariance_diag;
  nlohmann::json diagnostics = nlohmann::json::object();
};

class SampleSetStore {
 public:
  void append(PosteriorFrame frame);

  const std::vector<PosteriorFrame>& frames() const;
  const PosteriorFrame* latest() const;
  nlohmann::json toCheckpointJson(const std::string& run_id, const std::string& workflow_id, const std::string& object_id) const;

 private:
  std::vector<PosteriorFrame> frames_;
};

}  // namespace FlightEnvPlatformRuntime::estimation
