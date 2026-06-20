/**
 * @file RuntimeTensorInterpolator.cpp
 * @brief 实现张量或大场插值器的基础策略。
 *
 * 大概：这是 runtime 给大块数据留出的可插拔插值实现。
 * 具体：当前实现最简单的 nearest/hold 类选择，不在平台里写对象专属场插值算法。
 * 被谁使用：被 RuntimeTensorAlignment 和未来数据面扩展使用。
 * 使用谁：使用 RuntimeTensorInterpolator.hpp、RuntimeTimeTypes 和标准容器。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */

#include "FlightEnvPlatformRuntime/time/RuntimeTensorInterpolator.hpp"

#include "FlightEnvPlatformRuntime/time/RuntimeTensorAlignment.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace FlightEnvPlatformRuntime {
namespace {

std::string normalizedMethodId(const std::string& method_id) {
  if (method_id.empty()) {
    return "nearest";
  }
  if (method_id == "nearest_ref" || method_id == "nearest_sample") {
    return "nearest";
  }
  if (method_id == "operation_ref" || method_id == "lazy_operation_ref") {
    return "lazy_ref";
  }
  return method_id;
}

nlohmann::json tensorEvidence(
    const RuntimeTensorInterpolationRequest& request,
    const std::string& method_id,
    const std::string& status,
    double source_time_s,
    int sample_count) {
  return {
      {"method_id", method_id},
      {"operation", request.operation},
      {"channel_id", request.channel_id},
      {"status", status},
      {"target_time_s", request.target_time_s},
      {"source_time_s", source_time_s},
      {"window_start_s", request.window_start_s},
      {"window_end_s", request.window_end_s},
      {"sample_count", sample_count},
  };
}

const RuntimePortSample* nearestSample(
    const std::vector<RuntimePortSample>& samples,
    double target_time_s) {
  const RuntimePortSample* best = nullptr;
  double best_gap = 0.0;
  for (const auto& sample : samples) {
    const double gap = std::abs(sample.time_s - target_time_s);
    if (best == nullptr || gap < best_gap) {
      best = &sample;
      best_gap = gap;
    }
  }
  return best;
}

class NearestTensorInterpolator final : public RuntimeTensorInterpolator {
 public:
  std::string methodId() const override { return "nearest"; }

  RuntimeTensorInterpolationResult interpolate(
      const RuntimeTensorInterpolationRequest& request) const override {
    RuntimeTensorInterpolationResult result;
    result.method_id = methodId();
    result.sample_count = static_cast<int>(request.samples.size());
    const RuntimePortSample* selected = nearestSample(request.samples, request.target_time_s);
    if (selected == nullptr) {
      result.status = "no_tensor_samples";
      result.evidence = tensorEvidence(request, result.method_id, result.status, 0.0, result.sample_count);
      return result;
    }
    if (!RuntimeTensorAlignment::isTensorLike(selected->value)) {
      result.status = "unsupported_value_kind";
      result.source_time_s = selected->time_s;
      result.evidence = tensorEvidence(
          request,
          result.method_id,
          result.status,
          result.source_time_s,
          result.sample_count);
      return result;
    }

    result.available = true;
    result.status = "tensor_nearest_ref";
    result.source_time_s = selected->time_s;
    result.value = selected->value;
    if (result.value.is_object()) {
      result.value["runtime_tensor_alignment"] = {
          {"method_id", result.method_id},
          {"operation", request.operation},
          {"channel_id", request.channel_id},
          {"target_time_s", request.target_time_s},
          {"source_time_s", result.source_time_s},
          {"window_start_s", request.window_start_s},
          {"window_end_s", request.window_end_s},
          {"selected_source_node_id", selected->source_node_id},
          {"selected_source_port_id", selected->source_port_id},
          {"selected_iteration_index", selected->iteration_index},
          {"sample_count", result.sample_count},
      };
    }
    result.evidence = tensorEvidence(
        request,
        result.method_id,
        result.status,
        result.source_time_s,
        result.sample_count);
    return result;
  }
};

class LazyRefTensorInterpolator final : public RuntimeTensorInterpolator {
 public:
  std::string methodId() const override { return "lazy_ref"; }

  RuntimeTensorInterpolationResult interpolate(
      const RuntimeTensorInterpolationRequest& request) const override {
    RuntimeTensorInterpolationResult result;
    result.method_id = methodId();
    result.sample_count = static_cast<int>(request.samples.size());
    if (request.samples.empty()) {
      result.status = "no_tensor_samples";
      result.evidence = tensorEvidence(request, result.method_id, result.status, 0.0, result.sample_count);
      return result;
    }
    const bool all_tensor_like = std::all_of(
        request.samples.begin(),
        request.samples.end(),
        [](const RuntimePortSample& sample) {
          return RuntimeTensorAlignment::isTensorLike(sample.value);
        });
    if (!all_tensor_like) {
      result.status = "unsupported_value_kind";
      result.evidence = tensorEvidence(request, result.method_id, result.status, 0.0, result.sample_count);
      return result;
    }

    result.available = true;
    result.status = "tensor_lazy_operation_ref";
    result.source_time_s = request.target_time_s;
    if (request.operation == "linear_interpolate" && request.samples.size() >= 2) {
      result.value = RuntimeTensorAlignment::makeLinearOperationRef(
          request.samples.front(),
          request.samples.back(),
          request.target_time_s,
          request.channel_id);
    } else {
      result.value = RuntimeTensorAlignment::makeWindowReductionRef(
          request.samples,
          request.window_start_s,
          request.window_end_s,
          request.channel_id);
    }
    result.evidence = tensorEvidence(
        request,
        result.method_id,
        result.status,
        result.source_time_s,
        result.sample_count);
    return result;
  }
};

std::unique_ptr<RuntimeTensorInterpolator> createInterpolator(const std::string& method_id) {
  const std::string normalized = normalizedMethodId(method_id);
  if (normalized == "nearest") {
    return std::make_unique<NearestTensorInterpolator>();
  }
  if (normalized == "lazy_ref") {
    return std::make_unique<LazyRefTensorInterpolator>();
  }
  return nullptr;
}

}  // namespace

RuntimeTensorInterpolationResult RuntimeTensorInterpolatorRegistry::interpolate(
    const std::string& method_id,
    const RuntimeTensorInterpolationRequest& request) {
  const std::string normalized = normalizedMethodId(method_id);
  std::unique_ptr<RuntimeTensorInterpolator> interpolator = createInterpolator(normalized);
  if (interpolator) {
    return interpolator->interpolate(request);
  }
  RuntimeTensorInterpolationResult result;
  result.method_id = normalized;
  result.status = "unsupported_tensor_interpolation";
  result.sample_count = static_cast<int>(request.samples.size());
  result.evidence = tensorEvidence(request, normalized, result.status, 0.0, result.sample_count);
  return result;
}

std::vector<std::string> RuntimeTensorInterpolatorRegistry::supportedMethodIds() {
  return {"nearest", "lazy_ref"};
}

}  // namespace FlightEnvPlatformRuntime
