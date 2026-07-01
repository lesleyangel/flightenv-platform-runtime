#pragma once

/**
 * @file EstimatorMethodRegistry.hpp
 * @brief 系统级估计方法的注册和创建入口。
 *
 * 大概：把 PF、黑箱 UKF 等方法作为平台通用估计策略，而不是 workflow 普通算子。
 * 具体：注册表根据 estimation_plan.method.kind 创建方法实例；方法只处理数值向量和诊断，不读取对象 JSON。
 * 被谁使用：RuntimeEstimationService。
 * 使用谁：RuntimeObservationFrame、PosteriorFrame 和 nlohmann::json。
 */

#include "FlightEnvPlatformRuntime/estimation/RuntimeObservationInbox.hpp"
#include "FlightEnvPlatformRuntime/estimation/SampleSetStore.hpp"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime::estimation {

struct EstimatorStepRequest {
  RuntimeObservationFrame observation;
  const PosteriorFrame* previous = nullptr;
};

struct EstimatorStepResult {
  PosteriorFrame posterior;
  nlohmann::json evidence = nlohmann::json::object();
};

class IEstimatorMethod {
 public:
  virtual ~IEstimatorMethod() = default;
  virtual std::string kind() const = 0;
  virtual void configure(const nlohmann::json& method_config, std::size_t state_dim) = 0;
  virtual EstimatorStepResult step(const EstimatorStepRequest& request) = 0;
  virtual nlohmann::json snapshotState() const = 0;
  virtual void restoreState(const nlohmann::json& state) = 0;
};

class EstimatorMethodRegistry {
 public:
  static std::unique_ptr<IEstimatorMethod> create(const std::string& kind);
};

}  // namespace FlightEnvPlatformRuntime::estimation
