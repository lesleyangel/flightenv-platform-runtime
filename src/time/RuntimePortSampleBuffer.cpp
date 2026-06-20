/**
 * @file RuntimePortSampleBuffer.cpp
 * @brief 实现端口样本历史缓冲。
 *
 * 大概：这是输入对齐需要查询“最近值”和“时间窗口”的实际存储。
 * 具体：它按端口写入样本、按时间范围读取、修剪过期样本并返回对齐候选。
 * 被谁使用：被 RuntimeInputAlignment、RuntimeTimeScheduler 和样本缓冲测试使用。
 * 使用谁：使用 RuntimePortSampleBuffer.hpp、RuntimeTimeTypes、PortValue 和标准容器。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */

#include "FlightEnvPlatformRuntime/time/RuntimePortSampleBuffer.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace FlightEnvPlatformRuntime {
namespace {

double jsonDouble(const nlohmann::json& value, const std::string& key, double fallback = 0.0) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
    return fallback;
  }
  return value.at(key).get<double>();
}

double outputTimeS(const nlohmann::json& time_info) {
  if (time_info.is_object() && time_info.contains("public_output_time_s") &&
      time_info.at("public_output_time_s").is_number()) {
    return time_info.at("public_output_time_s").get<double>();
  }
  return jsonDouble(time_info.value("output_time_point", nlohmann::json::object()), "run_time_s", 0.0);
}

}  // namespace

nlohmann::json RuntimePortSample::evidence() const {
  return {
      {"channel_id", channel_id},
      {"source_node_id", source_node_id},
      {"source_port_id", source_port_id},
      {"time_s", time_s},
      {"iteration_index", iteration_index},
      {"value_kind", value.type_name()},
  };
}

std::string RuntimePortSampleBuffer::nodeChannel(const std::string& node_id) {
  return node_id;
}

std::string RuntimePortSampleBuffer::portChannel(
    const std::string& node_id,
    const std::string& port_id) {
  return port_id.empty() ? nodeChannel(node_id) : node_id + "/" + port_id;
}

void RuntimePortSampleBuffer::recordSample(RuntimePortSample sample) {
  auto& items = samples_by_channel_[sample.channel_id];
  items.push_back(std::move(sample));
  std::stable_sort(items.begin(), items.end(), [](const RuntimePortSample& lhs, const RuntimePortSample& rhs) {
    return lhs.time_s < rhs.time_s;
  });
}

void RuntimePortSampleBuffer::recordNodeOutput(
    const std::string& node_id,
    const nlohmann::json& execute_result,
    const nlohmann::json& time_info,
    int iteration_index) {
  RuntimePortSample node_sample;
  node_sample.channel_id = nodeChannel(node_id);
  node_sample.source_node_id = node_id;
  node_sample.time_s = outputTimeS(time_info);
  node_sample.iteration_index = iteration_index;
  node_sample.value = execute_result;
  node_sample.time_info = time_info;
  recordSample(std::move(node_sample));

  const nlohmann::json ports = execute_result.value("outputs", execute_result.value("output_ports", nlohmann::json::object()));
  if (!ports.is_object()) {
    return;
  }
  for (auto it = ports.begin(); it != ports.end(); ++it) {
    RuntimePortSample port_sample;
    port_sample.channel_id = portChannel(node_id, it.key());
    port_sample.source_node_id = node_id;
    port_sample.source_port_id = it.key();
    port_sample.time_s = outputTimeS(time_info);
    port_sample.iteration_index = iteration_index;
    port_sample.value = it.value();
    port_sample.time_info = time_info;
    recordSample(std::move(port_sample));
  }
}

std::vector<RuntimePortSample> RuntimePortSampleBuffer::samples(const std::string& channel_id) const {
  const auto found = samples_by_channel_.find(channel_id);
  if (found == samples_by_channel_.end()) {
    return {};
  }
  return found->second;
}

std::optional<RuntimePortSample> RuntimePortSampleBuffer::latestBeforeOrAt(
    const std::string& channel_id,
    double target_time_s,
    double max_staleness_s) const {
  const auto found = samples_by_channel_.find(channel_id);
  if (found == samples_by_channel_.end()) {
    return std::nullopt;
  }
  std::optional<RuntimePortSample> candidate;
  for (const auto& sample : found->second) {
    if (sample.time_s <= target_time_s + 1.0e-9) {
      candidate = sample;
    } else {
      break;
    }
  }
  if (!candidate.has_value()) {
    return std::nullopt;
  }
  if (max_staleness_s >= 0.0 && target_time_s - candidate->time_s > max_staleness_s + 1.0e-9) {
    return std::nullopt;
  }
  return candidate;
}

std::optional<RuntimePortSample> RuntimePortSampleBuffer::nearest(
    const std::string& channel_id,
    double target_time_s,
    double max_gap_s) const {
  const auto found = samples_by_channel_.find(channel_id);
  if (found == samples_by_channel_.end()) {
    return std::nullopt;
  }
  std::optional<RuntimePortSample> candidate;
  double best_gap = 0.0;
  for (const auto& sample : found->second) {
    const double gap = std::abs(sample.time_s - target_time_s);
    if (!candidate.has_value() || gap < best_gap) {
      candidate = sample;
      best_gap = gap;
    }
  }
  if (!candidate.has_value()) {
    return std::nullopt;
  }
  if (max_gap_s >= 0.0 && best_gap > max_gap_s + 1.0e-9) {
    return std::nullopt;
  }
  return candidate;
}

std::pair<std::optional<RuntimePortSample>, std::optional<RuntimePortSample>>
RuntimePortSampleBuffer::bracketing(
    const std::string& channel_id,
    double target_time_s) const {
  const auto found = samples_by_channel_.find(channel_id);
  if (found == samples_by_channel_.end()) {
    return {std::nullopt, std::nullopt};
  }
  std::optional<RuntimePortSample> before;
  std::optional<RuntimePortSample> after;
  for (const auto& sample : found->second) {
    if (sample.time_s <= target_time_s + 1.0e-9) {
      before = sample;
    }
    if (sample.time_s + 1.0e-9 >= target_time_s) {
      after = sample;
      break;
    }
  }
  return {before, after};
}

std::vector<RuntimePortSample> RuntimePortSampleBuffer::window(
    const std::string& channel_id,
    double start_time_s,
    double end_time_s) const {
  const auto found = samples_by_channel_.find(channel_id);
  if (found == samples_by_channel_.end()) {
    return {};
  }
  std::vector<RuntimePortSample> result;
  for (const auto& sample : found->second) {
    if (sample.time_s + 1.0e-9 >= start_time_s && sample.time_s <= end_time_s + 1.0e-9) {
      result.push_back(sample);
    }
  }
  return result;
}

nlohmann::json RuntimePortSampleBuffer::evidence() const {
  nlohmann::json channels = nlohmann::json::array();
  int sample_count = 0;
  for (const auto& item : samples_by_channel_) {
    sample_count += static_cast<int>(item.second.size());
    channels.push_back({
        {"channel_id", item.first},
        {"sample_count", item.second.size()},
    });
  }
  return {
      {"channel_count", samples_by_channel_.size()},
      {"sample_count", sample_count},
      {"channels", channels},
  };
}

}  // namespace FlightEnvPlatformRuntime
