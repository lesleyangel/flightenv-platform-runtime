/**
 * @file RuntimeInputAlignment.cpp
 * @brief 实现端口输入对齐策略。
 *
 * 大概：这是平台在算子执行前整理输入样本的地方。
 * 具体：它处理 hold_last、nearest、线性标量插值和简单窗口聚合，并把结果标记来源时间。
 * 被谁使用：被 RuntimeTimeScheduler、NativeWorkflowRunner 和输入对齐单测使用。
 * 使用谁：使用 RuntimePortSampleBuffer、RuntimeTensorAlignment、PortValue 和时间类型。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */

#include "FlightEnvPlatformRuntime/time/RuntimeInputAlignment.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeTensorAlignment.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeTensorInterpolator.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace FlightEnvPlatformRuntime {
namespace {

std::string jsonString(const nlohmann::json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

double jsonDouble(const nlohmann::json& value, const std::string& key, double fallback = 0.0) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
    return fallback;
  }
  return value.at(key).get<double>();
}

bool jsonBool(const nlohmann::json& value, const std::string& key, bool fallback = false) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean()) {
    return fallback;
  }
  return value.at(key).get<bool>();
}

RuntimeAlignmentStrategy parseStrategy(const nlohmann::json& raw) {
  const std::string explicit_strategy = jsonString(raw, "strategy");
  const std::string resampling = jsonString(raw, "input_resampling", "none");
  const std::string alignment = jsonString(raw, "alignment", "exact");
  const std::string strategy = !explicit_strategy.empty()
                                   ? explicit_strategy
                                   : (resampling != "none" ? resampling : alignment);
  if (strategy == "direct" || strategy == "exact" || strategy == "exact_or_substep") {
    return RuntimeAlignmentStrategy::Exact;
  }
  if (strategy == "hold_last") return RuntimeAlignmentStrategy::HoldLast;
  if (strategy == "nearest") return RuntimeAlignmentStrategy::Nearest;
  if (strategy == "interpolate" || strategy == "linear") return RuntimeAlignmentStrategy::Linear;
  if (strategy == "aggregate" || strategy == "integrate_window") return RuntimeAlignmentStrategy::IntegrateWindow;
  if (strategy == "independent") return RuntimeAlignmentStrategy::Independent;
  return RuntimeAlignmentStrategy::Unsupported;
}

std::string strategyName(RuntimeAlignmentStrategy strategy) {
  switch (strategy) {
    case RuntimeAlignmentStrategy::Exact: return "exact";
    case RuntimeAlignmentStrategy::HoldLast: return "hold_last";
    case RuntimeAlignmentStrategy::Nearest: return "nearest";
    case RuntimeAlignmentStrategy::Linear: return "linear";
    case RuntimeAlignmentStrategy::IntegrateWindow: return "integrate_window";
    case RuntimeAlignmentStrategy::Independent: return "independent";
    case RuntimeAlignmentStrategy::Unsupported: return "unsupported";
  }
  return "unsupported";
}

std::optional<double> scalarValue(const nlohmann::json& value) {
  if (value.is_number()) {
    return value.get<double>();
  }
  if (value.is_object() && value.contains("value") && value.at("value").is_number()) {
    return value.at("value").get<double>();
  }
  return std::nullopt;
}

nlohmann::json scalarLike(const nlohmann::json& original, double value) {
  if (original.is_object()) {
    nlohmann::json patched = original;
    patched["value"] = value;
    return patched;
  }
  return value;
}

double targetTimeS(const nlohmann::json& time_info) {
  return jsonDouble(
      time_info,
      "public_output_time_s",
      jsonDouble(time_info.value("output_time_point", nlohmann::json::object()), "run_time_s", 0.0));
}

double effectiveDeltaS(const nlohmann::json& time_info) {
  return jsonDouble(time_info, "effective_delta_t_s", jsonDouble(time_info, "delta_t_s", 0.0));
}

double durationSecondsNonNegative(RuntimeDuration duration) {
  if (duration.nanoseconds <= 0) {
    return 0.0;
  }
  return duration.seconds();
}

RuntimeInputAlignmentPolicy parsePolicy(const nlohmann::json& raw) {
  RuntimeInputAlignmentPolicy policy;
  policy.transition_id = jsonString(raw, "transition_id");
  policy.binding_id = jsonString(raw, "binding_id");
  policy.raw_strategy = jsonString(raw, "strategy");
  policy.rate_relation = jsonString(raw, "rate_relation");
  policy.from_rate_transition =
      !policy.transition_id.empty() || !policy.binding_id.empty() || !policy.rate_relation.empty();
  policy.requires_runtime_transition =
      jsonBool(raw, "requires_runtime_transition", policy.from_rate_transition);
  policy.source_period_s = jsonDouble(raw, "source_period_s", -1.0);
  policy.target_period_s = jsonDouble(raw, "target_period_s", -1.0);
  policy.max_age_s = jsonDouble(raw, "max_age_s", -1.0);
  policy.upstream_node_id = jsonString(raw, "upstream_node_id", jsonString(raw, "source_node_id"));
  policy.source_port_id = jsonString(raw, "source_port_id");
  policy.target_port_id = jsonString(raw, "target_port_id");
  policy.raw_alignment = jsonString(raw, "alignment", "exact");
  policy.raw_input_resampling = jsonString(raw, "input_resampling", "none");
  policy.tensor_interpolation = jsonString(raw, "tensor_interpolation", "nearest");
  policy.strategy = parseStrategy(raw);
  policy.max_staleness_s = jsonDouble(raw, "max_staleness_s", policy.max_age_s);
  policy.max_gap_s = jsonDouble(raw, "max_gap_s", policy.max_age_s);
  return policy;
}

RuntimeTensorInterpolationRequest tensorRequest(
    const std::string& operation,
    const std::string& channel,
    double target_time_s,
    double window_start_s,
    double window_end_s,
    std::vector<RuntimePortSample> samples) {
  RuntimeTensorInterpolationRequest request;
  request.operation = operation;
  request.channel_id = channel;
  request.target_time_s = target_time_s;
  request.window_start_s = window_start_s;
  request.window_end_s = window_end_s;
  request.samples = std::move(samples);
  return request;
}

void assignTensorResult(
    RuntimeAlignedInput& result,
    const RuntimeTensorInterpolationResult& tensor_result) {
  result.available = tensor_result.available;
  result.status = tensor_result.status;
  result.source_time_s = tensor_result.source_time_s;
  result.sample_count = tensor_result.sample_count;
  result.value = tensor_result.value;
  result.alignment_detail = tensor_result.evidence;
}

RuntimeAlignedInput alignOne(
    const RuntimeInputAlignmentPolicy& policy,
    double target_time_s,
    double window_start_s,
    const RuntimePortSampleBuffer& sample_buffer) {
  RuntimeAlignedInput result;
  result.policy = policy;
  result.target_time_s = target_time_s;
  result.window_start_s = window_start_s;
  result.window_end_s = target_time_s;

  if (policy.upstream_node_id.empty()) {
    result.status = "missing_upstream_node_id";
    return result;
  }
  if (policy.strategy == RuntimeAlignmentStrategy::Independent) {
    result.available = true;
    result.status = "independent";
    return result;
  }
  if (policy.strategy == RuntimeAlignmentStrategy::Unsupported) {
    result.status = "unsupported_strategy";
    return result;
  }

  const std::string channel = policy.source_port_id.empty()
                                  ? RuntimePortSampleBuffer::nodeChannel(policy.upstream_node_id)
                                  : RuntimePortSampleBuffer::portChannel(policy.upstream_node_id, policy.source_port_id);

  auto assignSample = [&](const std::optional<RuntimePortSample>& sample, const std::string& ok_status) {
    if (!sample.has_value()) {
      result.status = "no_matching_sample";
      return;
    }
    result.available = true;
    result.status = ok_status;
    result.source_time_s = sample->time_s;
    result.sample_count = 1;
    result.value = sample->value;
  };

  if (policy.strategy == RuntimeAlignmentStrategy::Exact) {
    assignSample(sample_buffer.latestBeforeOrAt(channel, target_time_s, 1.0e-9), "exact");
    return result;
  }
  if (policy.strategy == RuntimeAlignmentStrategy::HoldLast) {
    assignSample(sample_buffer.latestBeforeOrAt(channel, target_time_s, policy.max_staleness_s), "hold_last");
    return result;
  }
  if (policy.strategy == RuntimeAlignmentStrategy::Nearest) {
    assignSample(sample_buffer.nearest(channel, target_time_s, policy.max_gap_s), "nearest");
    return result;
  }
  if (policy.strategy == RuntimeAlignmentStrategy::Linear) {
    const auto bracket = sample_buffer.bracketing(channel, target_time_s);
    if (!bracket.first.has_value() || !bracket.second.has_value()) {
      result.status = "insufficient_samples";
      return result;
    }
    const auto lhs_value = scalarValue(bracket.first->value);
    const auto rhs_value = scalarValue(bracket.second->value);
    if (!lhs_value.has_value() || !rhs_value.has_value()) {
      if (RuntimeTensorAlignment::isTensorLike(bracket.first->value) &&
          RuntimeTensorAlignment::isTensorLike(bracket.second->value)) {
        assignTensorResult(
            result,
            RuntimeTensorInterpolatorRegistry::interpolate(
                policy.tensor_interpolation,
                tensorRequest(
                    "linear_interpolate",
                    channel,
                    target_time_s,
                    bracket.first->time_s,
                    bracket.second->time_s,
                    {*bracket.first, *bracket.second})));
        return result;
      }
      result.status = "unsupported_value_kind";
      result.sample_count = 2;
      return result;
    }
    const RuntimeDuration span = bracket.second->time - bracket.first->time;
    const RuntimeDuration offset = RuntimeTimePoint::fromSeconds(target_time_s) - bracket.first->time;
    const double alpha = span.nanoseconds == 0
                             ? 0.0
                             : static_cast<double>(offset.nanoseconds) /
                                   static_cast<double>(span.nanoseconds);
    result.available = true;
    result.status = "linear";
    result.source_time_s = target_time_s;
    result.sample_count = 2;
    result.value = scalarLike(bracket.first->value, *lhs_value + (*rhs_value - *lhs_value) * alpha);
    return result;
  }
  if (policy.strategy == RuntimeAlignmentStrategy::IntegrateWindow) {
    const std::vector<RuntimePortSample> samples = sample_buffer.window(channel, window_start_s, target_time_s);
    result.sample_count = static_cast<int>(samples.size());
    if (samples.empty()) {
      result.status = "insufficient_samples";
      return result;
    }
    double integral = 0.0;
    bool all_numeric = true;
    bool all_tensor_like = true;
    std::vector<std::optional<double>> scalar_values;
    scalar_values.reserve(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
      const auto current = scalarValue(samples[i].value);
      scalar_values.push_back(current);
      all_tensor_like = all_tensor_like && RuntimeTensorAlignment::isTensorLike(samples[i].value);
      if (!current.has_value()) {
        all_numeric = false;
        continue;
      }
    }
    if (!all_numeric) {
      if (all_tensor_like) {
        assignTensorResult(
            result,
            RuntimeTensorInterpolatorRegistry::interpolate(
                policy.tensor_interpolation,
                tensorRequest(
                    "integrate_window",
                    channel,
                    target_time_s,
                    window_start_s,
                    target_time_s,
                    samples)));
        return result;
      }
      result.status = "unsupported_value_kind";
      return result;
    }
    if (samples.size() == 1) {
      const RuntimeDuration window_duration =
          RuntimeTimePoint::fromSeconds(target_time_s) - RuntimeTimePoint::fromSeconds(window_start_s);
      integral = scalar_values.front().value_or(0.0) * durationSecondsNonNegative(window_duration);
    } else {
      for (std::size_t i = 1; i < samples.size(); ++i) {
        const double previous = scalar_values[i - 1].value_or(0.0);
        const double current = scalar_values[i].value_or(0.0);
        const double dt = durationSecondsNonNegative(samples[i].time - samples[i - 1].time);
        integral += 0.5 * (previous + current) * dt;
      }
    }
    result.available = true;
    result.status = "integrate_window";
    result.source_time_s = target_time_s;
    result.value = integral;
    return result;
  }

  result.status = "unsupported_strategy";
  return result;
}

}  // namespace

nlohmann::json RuntimeAlignedInput::evidence() const {
  return {
      {"transition_id", policy.transition_id},
      {"binding_id", policy.binding_id},
      {"upstream_node_id", policy.upstream_node_id},
      {"source_port_id", policy.source_port_id},
      {"target_port_id", policy.target_port_id},
      {"strategy", strategyName(policy.strategy)},
      {"raw_strategy", policy.raw_strategy},
      {"raw_alignment", policy.raw_alignment},
      {"raw_input_resampling", policy.raw_input_resampling},
      {"rate_relation", policy.rate_relation},
      {"from_rate_transition", policy.from_rate_transition},
      {"requires_runtime_transition", policy.requires_runtime_transition},
      {"source_period_s", policy.source_period_s},
      {"target_period_s", policy.target_period_s},
      {"max_age_s", policy.max_age_s},
      {"max_staleness_s", policy.max_staleness_s},
      {"max_gap_s", policy.max_gap_s},
      {"tensor_interpolation", policy.tensor_interpolation},
      {"available", available},
      {"status", status},
      {"target_time_s", target_time_s},
      {"source_time_s", source_time_s},
      {"window_start_s", window_start_s},
      {"window_end_s", window_end_s},
      {"sample_count", sample_count},
      {"alignment_detail", alignment_detail},
  };
}

bool RuntimeInputAlignmentResult::hasEvidence() const {
  return !inputs.empty();
}

bool RuntimeInputAlignmentResult::hasUnavailableRequiredInputs() const {
  for (const auto& input : inputs) {
    const bool required =
        input.policy.from_rate_transition &&
        input.policy.requires_runtime_transition &&
        input.policy.strategy != RuntimeAlignmentStrategy::Independent;
    if (required && !input.available) {
      return true;
    }
  }
  return false;
}

nlohmann::json RuntimeInputAlignmentResult::unavailableRequiredEvidence() const {
  nlohmann::json items = nlohmann::json::array();
  for (const auto& input : inputs) {
    const bool required =
        input.policy.from_rate_transition &&
        input.policy.requires_runtime_transition &&
        input.policy.strategy != RuntimeAlignmentStrategy::Independent;
    if (required && !input.available) {
      items.push_back(input.evidence());
    }
  }
  return items;
}

nlohmann::json RuntimeInputAlignmentResult::evidence() const {
  nlohmann::json items = nlohmann::json::array();
  for (const auto& input : inputs) {
    items.push_back(input.evidence());
  }
  return items;
}

RuntimeInputAlignmentResult RuntimeInputAlignment::alignNodeInputs(
    const nlohmann::json& node,
    const nlohmann::json& time_info,
    const RuntimePortSampleBuffer& sample_buffer) {
  RuntimeInputAlignmentResult result;
  nlohmann::json alignments = nlohmann::json::array();
  if (node.contains("rate_transitions") && node.at("rate_transitions").is_array() &&
      !node.at("rate_transitions").empty()) {
    alignments = node.at("rate_transitions");
  } else {
    alignments = time_info.value("input_alignment", node.value("input_alignment", nlohmann::json::array()));
  }
  if (!alignments.is_array()) {
    return result;
  }
  const RuntimeTimePoint target_time_point = RuntimeTimePoint::fromSeconds(targetTimeS(time_info));
  const RuntimeDuration effective_delta =
      RuntimeDuration::fromSeconds(std::max(0.0, effectiveDeltaS(time_info)));
  const RuntimeTimePoint window_start_point =
      RuntimeTimePoint::fromNanoseconds(target_time_point.nanoseconds - effective_delta.nanoseconds);
  const double target_time = target_time_point.seconds();
  const double window_start = window_start_point.seconds();
  for (const auto& raw : alignments) {
    if (!raw.is_object()) {
      continue;
    }
    result.inputs.push_back(alignOne(parsePolicy(raw), target_time, window_start, sample_buffer));
  }
  return result;
}

void RuntimeInputAlignment::applyAlignedInputs(
    const RuntimeInputAlignmentResult& result,
    nlohmann::json& upstream) {
  if (!upstream.is_object()) {
    upstream = nlohmann::json::object();
  }
  if (!upstream.contains("runtime_input_alignment") || !upstream.at("runtime_input_alignment").is_array()) {
    upstream["runtime_input_alignment"] = nlohmann::json::array();
  }
  for (const auto& input : result.inputs) {
    upstream["runtime_input_alignment"].push_back(input.evidence());
    if (!input.available || input.policy.upstream_node_id.empty() || input.value.is_null()) {
      continue;
    }
    if (!upstream.contains(input.policy.upstream_node_id)) {
      upstream[input.policy.upstream_node_id] = input.value;
    }
    if (!input.policy.target_port_id.empty()) {
      if (!upstream.contains("input_ports") || !upstream.at("input_ports").is_object()) {
        upstream["input_ports"] = nlohmann::json::object();
      }
      if (!upstream["input_ports"].contains(input.policy.target_port_id)) {
        upstream["input_ports"][input.policy.target_port_id] = input.value;
      }
    }
  }
}

}  // namespace FlightEnvPlatformRuntime
