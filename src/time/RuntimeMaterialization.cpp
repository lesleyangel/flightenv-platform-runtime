/**
 * @file RuntimeMaterialization.cpp
 * @brief 实现公开输出帧物化策略。
 *
 * 大概：这是把内部运行结果变成外部可见 frame、timeline 或 evidence 条目的地方。
 * 具体：它负责判断公开 tick、选择可展示输出、填充公开时间和来源信息。
 * 被谁使用：被 RuntimeTimeScheduler、NativeWorkflowRunner 和物化策略测试使用。
 * 使用谁：使用 RuntimeMaterialization.hpp、RuntimeTimeTypes、PortValue 和 PDK frame/evidence 类型。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */

#include "FlightEnvPlatformRuntime/time/RuntimeMaterialization.hpp"

#include <cstdint>
#include <iomanip>
#include <sstream>

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

int jsonInt(const nlohmann::json& value, const std::string& key, int fallback = 0) {
  if (!value.is_object() || !value.contains(key)) {
    return fallback;
  }
  const auto& item = value.at(key);
  if (item.is_number_integer()) return item.get<int>();
  if (item.is_number()) return static_cast<int>(item.get<double>());
  return fallback;
}

std::uint64_t jsonUInt64(const nlohmann::json& value, const std::string& key, std::uint64_t fallback = 0) {
  if (!value.is_object() || !value.contains(key)) {
    return fallback;
  }
  const auto& item = value.at(key);
  if (item.is_number_unsigned()) return item.get<std::uint64_t>();
  if (item.is_number_integer()) {
    const auto signed_value = item.get<std::int64_t>();
    return signed_value > 0 ? static_cast<std::uint64_t>(signed_value) : fallback;
  }
  if (item.is_number()) {
    const auto double_value = item.get<double>();
    return double_value > 0.0 ? static_cast<std::uint64_t>(double_value) : fallback;
  }
  return fallback;
}

bool jsonBool(const nlohmann::json& value, const std::string& key, bool fallback = false) {
  return value.is_object() && value.contains(key) && value.at(key).is_boolean()
             ? value.at(key).get<bool>()
             : fallback;
}

std::uint64_t fnv64(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string digestJson(const nlohmann::json& value) {
  const std::string text = value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  std::ostringstream out;
  out << "fnv64:" << std::hex << std::setw(16) << std::setfill('0') << fnv64(text);
  return out.str();
}

nlohmann::json summarizeJson(const nlohmann::json& value) {
  const std::string text = value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  nlohmann::json summary = {
      {"type", value.type_name()},
      {"byte_size", static_cast<int>(text.size())},
      {"digest_algorithm", "fnv64"},
      {"digest", digestJson(value)},
  };
  if (value.is_object()) {
    nlohmann::json keys = nlohmann::json::array();
    int count = 0;
    for (auto it = value.begin(); it != value.end() && count < 32; ++it, ++count) {
      keys.push_back(it.key());
    }
    summary["keys"] = keys;
  }
  if (value.is_array()) {
    summary["array_size"] = value.size();
  }
  return summary;
}

void applyArtifactRef(nlohmann::json& entry, const nlohmann::json& value) {
  if (!value.is_object() || !value.contains("artifact_ref") || !value.at("artifact_ref").is_object()) {
    return;
  }
  const nlohmann::json& ref = value.at("artifact_ref");
  entry["representation"] = "artifact_ref";
  entry["artifact_uri"] = jsonString(ref, "uri", jsonString(ref, "path"));
  entry["ref"] = jsonString(ref, "uri", jsonString(ref, "path"));
  entry["node_count"] = jsonInt(ref, "node_count", jsonInt(value, "node_count", 0));
  entry["field_name"] = jsonString(ref, "field_name", jsonString(value, "field_name"));
  entry["mesh_ref"] = jsonString(ref, "mesh_ref", jsonString(value, "mesh_ref"));
  entry["shape"] = ref.value("shape", value.value("shape", nlohmann::json::array()));
}

void applyTensorRef(nlohmann::json& entry, const nlohmann::json& value) {
  if (!value.is_object() || !value.contains("tensor_ref") || !value.at("tensor_ref").is_object()) {
    return;
  }
  const nlohmann::json& ref = value.at("tensor_ref");
  entry["representation"] = "tensor_ref";
  entry["ref"] = jsonString(ref, "uri", jsonString(ref, "path", jsonString(ref, "id")));
  entry["shape"] = ref.value("shape", value.value("shape", nlohmann::json::array()));
  entry["typed_schema_id"] = jsonString(ref, "schema_id", jsonString(entry, "typed_schema_id"));
  entry["typed_dto_name"] = jsonString(ref, "dto_name", jsonString(entry, "typed_dto_name"));
  entry["buffer_layout_id"] = jsonString(
      ref,
      "layout_id",
      jsonString(ref, "buffer_layout_id", jsonString(entry, "buffer_layout_id")));
  entry["buffer_bytes"] = jsonUInt64(ref, "byte_size", jsonUInt64(ref, "buffer_bytes", 0));
  entry["zero_copy_eligible"] = jsonBool(ref, "zero_copy_eligible", jsonBool(entry, "zero_copy_eligible", false));
}

void applyTypedBufferRef(nlohmann::json& entry, const nlohmann::json& value) {
  if (!value.is_object() || !value.contains("typed_buffer_ref") || !value.at("typed_buffer_ref").is_object()) {
    return;
  }
  const nlohmann::json& ref = value.at("typed_buffer_ref");
  entry["representation"] = "typed_buffer_ref";
  entry["typed_buffer_ref"] = jsonString(ref, "uri", jsonString(ref, "path", jsonString(ref, "id")));
  entry["ref"] = jsonString(entry, "typed_buffer_ref");
  entry["shape"] = ref.value("shape", value.value("shape", nlohmann::json::array()));
  entry["typed_schema_id"] = jsonString(ref, "schema_id", jsonString(entry, "typed_schema_id"));
  entry["typed_dto_name"] = jsonString(ref, "dto_name", jsonString(entry, "typed_dto_name"));
  entry["buffer_layout_id"] = jsonString(
      ref,
      "layout_id",
      jsonString(ref, "buffer_layout_id", jsonString(entry, "buffer_layout_id")));
  entry["buffer_bytes"] = jsonUInt64(ref, "byte_size", jsonUInt64(ref, "buffer_bytes", 0));
  entry["zero_copy_eligible"] = jsonBool(ref, "zero_copy_eligible", jsonBool(entry, "zero_copy_eligible", false));
}

void applyTensorAlignmentRef(nlohmann::json& entry, const nlohmann::json& value) {
  if (!value.is_object() || !value.contains("tensor_alignment_ref") ||
      !value.at("tensor_alignment_ref").is_object()) {
    return;
  }
  const nlohmann::json& ref = value.at("tensor_alignment_ref");
  entry["representation"] = "tensor_operation_ref";
  entry["tensor_alignment_ref"] = ref;
  entry["ref"] = jsonString(ref, "operation_id", jsonString(ref, "uri"));
  entry["lazy_load"] = true;
  entry["materialization"] = "lazy_tensor_operation";
}

void applyTypedPayloadRef(nlohmann::json& entry, const nlohmann::json& value) {
  if (value.is_object() && value.contains("typed_buffer_ref") && value.at("typed_buffer_ref").is_object()) {
    return;
  }
  if (value.is_object() && value.contains("typed_payload_ref") && value.at("typed_payload_ref").is_string()) {
    const std::string ref = value.at("typed_payload_ref").get<std::string>();
    if (!ref.empty()) {
      entry["representation"] = "typed_payload_ref";
      entry["typed_payload_ref"] = ref;
      entry["ref"] = ref;
    }
  }
}

nlohmann::json makeEntry(
    const std::string& direction,
    const nlohmann::json& spec,
    const nlohmann::json& data_plane_info,
    const nlohmann::json& value,
    const nlohmann::json& time_info,
    const nlohmann::json& point,
    int iteration_index) {
  const nlohmann::json summary = summarizeJson(value);
  const nlohmann::json statistics = value.is_object() ? value.value("statistics", nlohmann::json::object())
                                                      : nlohmann::json::object();
  const nlohmann::json typed_contract = spec.value("typed_io_contract", nlohmann::json::object());
  nlohmann::json entry = {
      {"node_id", jsonString(spec, "node_id", jsonString(data_plane_info, "node_id"))},
      {"operator_id", jsonString(spec, "operator_id", jsonString(data_plane_info, "operator_id"))},
      {"direction", direction},
      {"port_id", jsonString(spec, "port_id")},
      {"contract_id", jsonString(spec, "contract_id")},
      {"typed_schema_id", jsonString(typed_contract, "schema_id")},
      {"typed_dto_name", jsonString(
          typed_contract,
          direction == "input" ? "input_dto" : "output_dto",
          jsonString(typed_contract, "dto_name", jsonString(typed_contract, "type_name")))},
      {"typed_payload_ref", ""},
      {"typed_buffer_ref", ""},
      {"buffer_layout_id", jsonString(typed_contract, "buffer_layout_id")},
      {"value_kind", jsonString(spec, "value_kind", jsonString(value, "value_kind"))},
      {"role", jsonString(spec, "role", jsonString(value, "role", jsonString(value, "display_role")))},
      {"buffer_bytes", 0},
      {"zero_copy_eligible", jsonBool(typed_contract, "zero_copy_eligible", false)},
      {"representation", "inline_json"},
      {"ref", jsonString(spec, "data_ref")},
      {"checksum", summary.value("digest", "")},
      {"layout_ref", jsonString(spec, "layout_ref")},
      {"shape", nlohmann::json::array()},
      {"artifact_uri", ""},
      {"node_count", 0},
      {"field_name", jsonString(value, "field_name")},
      {"component_id", jsonString(value, "component_id")},
      {"mesh_ref", jsonString(value, "mesh_ref")},
      {"unit", jsonString(value, "unit")},
      {"time_point", point},
      {"time_summary", RuntimeMaterialization::timeSummary(time_info, point)},
      {"statistics", statistics},
      {"lazy_load", false},
      {"inline_byte_size", summary.value("byte_size", 0)},
      {"evidence_ref", "runtime_node_snapshot.json"},
      {"loop_iteration_index", iteration_index},
  };
  const nlohmann::json time_summary = RuntimeMaterialization::timeSummary(time_info, point);
  entry["public_time_s"] = time_summary.value("public_output_time_s", 0.0);
  entry["effective_delta_t_s"] = time_summary.value("effective_delta_t_s", 0.0);
  entry["output_period_s"] = time_summary.value("output_period_s", 0.0);

  applyArtifactRef(entry, value);
  applyTensorRef(entry, value);
  applyTypedBufferRef(entry, value);
  applyTensorAlignmentRef(entry, value);
  applyTypedPayloadRef(entry, value);
  return entry;
}

nlohmann::json valueForPort(const nlohmann::json& values, const std::string& port_id) {
  if (values.is_object() && !port_id.empty() && values.contains(port_id)) {
    return values.at(port_id);
  }
  if (values.is_object() && values.contains("input_ports") && values.at("input_ports").is_object() &&
      !port_id.empty() && values.at("input_ports").contains(port_id)) {
    return values.at("input_ports").at(port_id);
  }
  if (values.is_object()) {
    return values;
  }
  return nlohmann::json::object();
}

}  // namespace

nlohmann::json RuntimeMaterialization::timeSummary(
    const nlohmann::json& time_info,
    const nlohmann::json& point) {
  return {
      {"public_output_time_s", jsonDouble(time_info, "public_output_time_s", jsonDouble(point, "run_time_s", 0.0))},
      {"effective_delta_t_s", jsonDouble(time_info, "effective_delta_t_s", jsonDouble(time_info, "delta_t_s", 0.0))},
      {"output_period_s", jsonDouble(time_info, "output_period_s", 0.0)},
      {"held", jsonBool(time_info, "held", false)},
      {"dispatch_reason", jsonString(time_info, "dispatch_reason")},
      {"public_tick", time_info.value("public_tick", nlohmann::json::object())},
  };
}

nlohmann::json RuntimeMaterialization::makeDataPlaneEntries(
    const nlohmann::json& data_plane_info,
    const nlohmann::json& input_payload,
    const nlohmann::json& output_payload,
    const nlohmann::json& time_info,
    int iteration_index) {
  nlohmann::json entries = nlohmann::json::array();
  const auto add_entries = [&](const std::string& direction,
                               const nlohmann::json& specs,
                               const nlohmann::json& values,
                               const nlohmann::json& point) {
    if (!specs.is_array()) {
      return;
    }
    for (const auto& spec : specs) {
      if (!spec.is_object()) {
        continue;
      }
      const std::string port_id = jsonString(spec, "port_id");
      const nlohmann::json value = valueForPort(values, port_id);
      entries.push_back(makeEntry(direction, spec, data_plane_info, value, time_info, point, iteration_index));
    }
  };

  add_entries("input", data_plane_info.value("inputs", nlohmann::json::array()), input_payload,
              time_info.value("time_point", nlohmann::json::object()));
  add_entries("output", data_plane_info.value("outputs", nlohmann::json::array()),
              output_payload.value("outputs", nlohmann::json::object()),
              time_info.value("output_time_point", time_info.value("time_point", nlohmann::json::object())));
  return entries;
}

}  // namespace FlightEnvPlatformRuntime
