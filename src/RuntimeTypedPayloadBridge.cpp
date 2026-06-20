/**
 * @file RuntimeTypedPayloadBridge.cpp
 * @brief 实现 typed payload 引用物化。
 *
 * 大概：把 NativeWorkflowRunner 中的 typed IO 桥接逻辑拆出来，避免主 runner 继续变大。
 * 具体：默认不再把 inline JSON 自动桥接为 typed buffer；只有设置
 * `FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE=1` 时，才写出兼容格式
 * `json_typed_payload.v1`，供迁移诊断使用。
 * 被谁使用：被 NativeWorkflowRunner 在执行节点后、data-plane 物化前调用。
 * 使用谁：使用编译后的 data-plane 端口 spec、运行时间信息和文件系统。
 */

#include "FlightEnvPlatformRuntime/RuntimeTypedPayloadBridge.hpp"

#include "FlightEnvPlatformRuntime/RuntimeTypedBufferStore.hpp"
#include "FlightEnvPlatformRuntime/RuntimeZeroCopyPolicy.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace FlightEnvPlatformRuntime {
namespace {

std::string nowUtcIso() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const std::time_t time = clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

std::string jsonString(const nlohmann::json& value,
                       const std::string& key,
                       const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

std::string stableId(std::string text) {
  std::replace_if(text.begin(), text.end(), [](unsigned char c) {
    return !(std::isalnum(c) || c == '_' || c == '-' || c == '.');
  }, '_');
  return text;
}

std::uint64_t stableHash64(const std::string& text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string shortStorageId(const std::string& prefix, const std::string& text) {
  std::ostringstream oss;
  oss << prefix << '_' << std::hex << std::setw(16) << std::setfill('0') << stableHash64(text);
  return oss.str();
}

bool typedBufferTraceEnabled() {
  const char* value = std::getenv("FLIGHTENV_TYPED_BUFFER_TRACE");
  if (!value) {
    return false;
  }
  const std::string text(value);
  return !text.empty() && text != "0" && text != "false" && text != "FALSE";
}

void appendBridgeTrace(const fs::path& run_dir, const std::string& message) {
  if (run_dir.empty() || !typedBufferTraceEnabled()) {
    return;
  }
  std::error_code ec;
  fs::create_directories(run_dir, ec);
  std::ofstream out(run_dir / "typed_buffer_bridge_trace.log", std::ios::app);
  if (!out) {
    return;
  }
  out << nowUtcIso() << ' ' << message << '\n';
}

std::string runtimeExecutionTag(const nlohmann::json& time_info,
                                int iteration_index,
                                const std::string& node_id) {
  const nlohmann::json runtime_event = time_info.value("runtime_event", nlohmann::json::object());
  const std::string event_id = jsonString(runtime_event, "event_id");
  if (!event_id.empty()) {
    return stableId(event_id);
  }
  return "step_" + std::to_string(iteration_index) + "." + stableId(node_id);
}

bool hasPayloadRef(const nlohmann::json& value) {
  if (!value.is_object()) {
    return false;
  }
  if (value.contains("artifact_ref") && value.at("artifact_ref").is_object()) return true;
  if (value.contains("tensor_ref") && value.at("tensor_ref").is_object()) return true;
  if (value.contains("typed_buffer_ref") && value.at("typed_buffer_ref").is_object()) return true;
  if (value.contains("typed_payload_ref") && value.at("typed_payload_ref").is_string() &&
      !value.at("typed_payload_ref").get<std::string>().empty()) {
    return true;
  }
  return false;
}

}  // namespace

bool typedIoJsonForbidden(const nlohmann::json& port_spec) {
  return zeroCopyRequiredForPort(port_spec);
}

nlohmann::json materializeTypedOutputPayloads(const fs::path& run_dir,
                                             const nlohmann::json& data_plane_info,
                                             nlohmann::json execute_result,
                                             const nlohmann::json& time_info,
                                             int iteration_index,
                                             const std::string& typed_buffer_persistence) {
  if (!allowInlineJsonTypedPayloadBridge()) {
    return execute_result;
  }
  if (!execute_result.is_object() || !execute_result.contains("outputs") ||
      !execute_result.at("outputs").is_object()) {
    return execute_result;
  }
  const nlohmann::json output_specs = data_plane_info.value("outputs", nlohmann::json::array());
  if (!output_specs.is_array() || output_specs.empty()) {
    return execute_result;
  }

  nlohmann::json& outputs = execute_result["outputs"];
  const std::string node_id = jsonString(data_plane_info, "node_id");
  const std::string event_tag = runtimeExecutionTag(time_info, iteration_index, node_id);
  const RuntimeTypedBufferPersistenceMode persistence_mode =
      resolveRuntimeTypedBufferPersistence(typed_buffer_persistence);

  for (const auto& spec : output_specs) {
    if (!typedIoJsonForbidden(spec)) {
      continue;
    }
    const std::string port_id = jsonString(spec, "port_id");
    if (port_id.empty() || !outputs.contains(port_id) || !outputs.at(port_id).is_object()) {
      continue;
    }
    nlohmann::json& payload = outputs[port_id];
    if (hasPayloadRef(payload)) {
      continue;
    }

    const nlohmann::json typed_contract = spec.value("typed_io_contract", nlohmann::json::object());
    const std::string typed_schema_id = jsonString(typed_contract, "schema_id");
    const std::string typed_dto_name =
        jsonString(typed_contract,
                   "output_dto",
                   jsonString(typed_contract, "dto_name", jsonString(typed_contract, "type_name")));
    const std::string storage_node_key = shortStorageId("n", node_id);
    const std::string storage_port_key = shortStorageId("p", port_id);
    const nlohmann::json typed_payload = {
        {"schema_version", "flightenv.platform.typed_payload_ref.v1"},
        {"created_at_utc", nowUtcIso()},
        {"storage_node_key", storage_node_key},
        {"storage_port_key", storage_port_key},
        {"node_id", node_id},
        {"operator_id", jsonString(data_plane_info, "operator_id")},
        {"port_id", port_id},
        {"contract_id", jsonString(spec, "contract_id")},
        {"frame_contract", jsonString(spec, "frame_contract")},
        {"typed_schema_id", typed_schema_id},
        {"typed_dto_name", typed_dto_name},
        {"runtime_event_tag", event_tag},
        {"time_info", time_info},
        {"payload", payload},
    };
    const std::string bytes_text = typed_payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    RuntimeTypedBufferRequest buffer_request;
    buffer_request.run_dir = run_dir;
    buffer_request.node_id = node_id;
    buffer_request.port_id = port_id;
    buffer_request.schema_id = typed_schema_id;
    buffer_request.dto_name = typed_dto_name;
    buffer_request.layout_id = jsonString(typed_contract, "buffer_layout_id", typed_schema_id);
    buffer_request.format = "json_typed_payload.v1";
    buffer_request.persistence_mode = persistence_mode;
    buffer_request.write_shadow_artifact =
        runtimeTypedBufferPersistenceWritesShadow(persistence_mode);
    buffer_request.bytes.assign(bytes_text.begin(), bytes_text.end());
    appendBridgeTrace(run_dir,
                      "allocate_begin node=" + node_id + " port=" + port_id +
                          " bytes=" + std::to_string(buffer_request.bytes.size()));
    const RuntimeTypedBufferAllocation allocation =
        RuntimeTypedBufferStore::instance().allocate(std::move(buffer_request));
    appendBridgeTrace(run_dir,
                      "allocate_end node=" + node_id + " port=" + port_id +
                          " buffer_id=" + allocation.buffer_id);

    appendBridgeTrace(run_dir, "assign_ref_begin node=" + node_id + " port=" + port_id);
    payload["typed_buffer_ref"] = allocation.ref;
    payload["typed_schema_id"] = typed_schema_id;
    payload["typed_dto_name"] = typed_dto_name;
    appendBridgeTrace(run_dir, "assign_ref_end node=" + node_id + " port=" + port_id);
  }
  return execute_result;
}

}  // namespace FlightEnvPlatformRuntime
