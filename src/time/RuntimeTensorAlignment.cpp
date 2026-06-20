/**
 * @file RuntimeTensorAlignment.cpp
 * @brief 实现张量或大场引用的时间对齐入口。
 *
 * 大概：这是把 tensor ref 按目标时间选出合适样本的实现层。
 * 具体：当前先调用就近策略，后续可替换为重采样、窗口 reducer 或对象包提供的插值器。
 * 被谁使用：被 RuntimeInputAlignment 和 tensor 对齐测试使用。
 * 使用谁：使用 RuntimeTensorInterpolator、PortValue/DataPlane 引用和时间类型。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */

#include "FlightEnvPlatformRuntime/time/RuntimeTensorAlignment.hpp"

#include <cstdint>
#include <iomanip>
#include <sstream>

namespace FlightEnvPlatformRuntime {
namespace {

std::uint64_t fnv64(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string stableOperationId(const std::string& operation, const nlohmann::json& payload) {
  std::ostringstream out;
  out << "tensor_alignment." << operation << "." << std::hex << std::setw(16)
      << std::setfill('0') << fnv64(payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
  return out.str();
}

std::string jsonString(const nlohmann::json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

nlohmann::json refObject(const nlohmann::json& value) {
  if (!value.is_object()) {
    return nlohmann::json::object();
  }
  for (const std::string key : {"tensor_ref", "typed_buffer_ref", "artifact_ref"}) {
    if (value.contains(key) && value.at(key).is_object()) {
      nlohmann::json ref = value.at(key);
      ref["ref_kind"] = key;
      return ref;
    }
  }
  if (value.contains("typed_payload_ref") && value.at("typed_payload_ref").is_string()) {
    return {
        {"ref_kind", "typed_payload_ref"},
        {"uri", value.at("typed_payload_ref").get<std::string>()},
    };
  }
  for (const std::string key : {"artifact_uri", "artifact_path", "uri", "path"}) {
    if (value.contains(key) && value.at(key).is_string() && !value.at(key).get<std::string>().empty()) {
      return {
          {"ref_kind", key},
          {"uri", value.at(key).get<std::string>()},
      };
    }
  }
  return nlohmann::json::object();
}

nlohmann::json sampleInputRef(const RuntimePortSample& sample) {
  nlohmann::json ref = refObject(sample.value);
  return {
      {"channel_id", sample.channel_id},
      {"source_node_id", sample.source_node_id},
      {"source_port_id", sample.source_port_id},
      {"time_s", sample.time_s},
      {"iteration_index", sample.iteration_index},
      {"ref", ref},
      {"shape", ref.value("shape", sample.value.value("shape", nlohmann::json::array()))},
      {"dtype", jsonString(ref, "dtype", jsonString(sample.value, "dtype"))},
      {"mesh_ref", jsonString(ref, "mesh_ref", jsonString(sample.value, "mesh_ref"))},
      {"field_name", jsonString(ref, "field_name", jsonString(sample.value, "field_name"))},
      {"component_id", jsonString(ref, "component_id", jsonString(sample.value, "component_id"))},
      {"unit", jsonString(ref, "unit", jsonString(sample.value, "unit"))},
  };
}

nlohmann::json operationValue(
    const std::string& operation,
    const std::string& channel_id,
    double target_time_s,
    double window_start_s,
    double window_end_s,
    nlohmann::json inputs) {
  nlohmann::json seed = {
      {"operation", operation},
      {"channel_id", channel_id},
      {"target_time_s", target_time_s},
      {"window_start_s", window_start_s},
      {"window_end_s", window_end_s},
      {"inputs", inputs},
  };
  const std::string operation_id = stableOperationId(operation, seed);
  nlohmann::json first_ref = nlohmann::json::object();
  if (inputs.is_array() && !inputs.empty() && inputs.front().is_object()) {
    first_ref = inputs.front().value("ref", nlohmann::json::object());
  }
  const nlohmann::json operation_ref = {
      {"representation", "tensor_operation_ref"},
      {"operation_id", operation_id},
      {"operation", operation},
      {"channel_id", channel_id},
      {"target_time_s", target_time_s},
      {"window_start_s", window_start_s},
      {"window_end_s", window_end_s},
      {"input_count", inputs.is_array() ? inputs.size() : 0},
      {"inputs", inputs},
      {"materialization", "lazy"},
      {"requires_interpolator", true},
  };
  return {
      {"value_kind", "aligned_tensor_ref"},
      {"tensor_alignment_ref", operation_ref},
      {"tensor_ref",
       {
           {"representation", "tensor_ref"},
           {"id", operation_id},
           {"uri", "runtime://tensor_alignment/" + operation_id},
           {"source_operation", operation},
           {"shape", first_ref.value("shape", nlohmann::json::array())},
           {"dtype", jsonString(first_ref, "dtype")},
           {"layout_id", jsonString(first_ref, "layout_id", jsonString(first_ref, "buffer_layout_id"))},
           {"zero_copy_eligible", false},
       }},
  };
}

}  // namespace

bool RuntimeTensorAlignment::isTensorLike(const nlohmann::json& value) {
  if (!value.is_object()) {
    return false;
  }
  if (value.contains("tensor_ref") && value.at("tensor_ref").is_object()) return true;
  if (value.contains("typed_buffer_ref") && value.at("typed_buffer_ref").is_object()) return true;
  if (value.contains("artifact_ref") && value.at("artifact_ref").is_object()) return true;
  if (value.contains("typed_payload_ref") && value.at("typed_payload_ref").is_string() &&
      !value.at("typed_payload_ref").get<std::string>().empty()) {
    return true;
  }
  for (const std::string key : {"artifact_uri", "artifact_path", "uri", "path"}) {
    if (value.contains(key) && value.at(key).is_string() && !value.at(key).get<std::string>().empty()) {
      return true;
    }
  }
  return false;
}

nlohmann::json RuntimeTensorAlignment::makeLinearOperationRef(
    const RuntimePortSample& before,
    const RuntimePortSample& after,
    double target_time_s,
    const std::string& channel_id) {
  nlohmann::json inputs = nlohmann::json::array({sampleInputRef(before), sampleInputRef(after)});
  return operationValue("linear_interpolate", channel_id, target_time_s, before.time_s, after.time_s, inputs);
}

nlohmann::json RuntimeTensorAlignment::makeWindowReductionRef(
    const std::vector<RuntimePortSample>& samples,
    double window_start_s,
    double window_end_s,
    const std::string& channel_id) {
  nlohmann::json inputs = nlohmann::json::array();
  for (const auto& sample : samples) {
    inputs.push_back(sampleInputRef(sample));
  }
  return operationValue("integrate_window", channel_id, window_end_s, window_start_s, window_end_s, inputs);
}

}  // namespace FlightEnvPlatformRuntime
