/**
 * @file RuntimeZeroCopyPolicy.cpp
 * @brief 实现 Runtime 热路径零拷贝合规检查。
 *
 * 大概：把 JSON hot path 禁止规则从 runner/bridge 中抽出来，避免以后每个执行器各写一套。
 * 具体：只看平台通用端口声明和 payload 引用形态，不读取对象包物理语义。
 * 被谁使用：NativeWorkflowRunner 在 adapter 执行后调用，RuntimeTypedPayloadBridge 用它判断
 * 是否允许迁移兼容桥接。
 * 使用谁：nlohmann::json、cstdlib 和 stdexcept。
 */

#include "FlightEnvPlatformRuntime/RuntimeZeroCopyPolicy.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace FlightEnvPlatformRuntime {
namespace {

bool jsonBool(const nlohmann::json& value, const std::string& key, bool fallback = false) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean()) {
    return fallback;
  }
  return value.at(key).get<bool>();
}

std::string jsonString(const nlohmann::json& value,
                       const std::string& key,
                       const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

bool envFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  if (!value) {
    return false;
  }
  const std::string text(value);
  return !text.empty() && text != "0" && text != "false" && text != "FALSE";
}

std::string envString(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

std::string normalizedModeText(std::string value) {
  value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
                return std::isspace(c) != 0;
              }),
              value.end());
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool hasObjectRef(const nlohmann::json& value, const char* key) {
  return value.is_object() && value.contains(key) && value.at(key).is_object();
}

bool hasNonEmptyStringRef(const nlohmann::json& value, const char* key) {
  return value.is_object() && value.contains(key) && value.at(key).is_string() &&
         !value.at(key).get<std::string>().empty();
}

bool portHasTypedContract(const nlohmann::json& port_spec) {
  return port_spec.is_object() && port_spec.contains("typed_io_contract") &&
         port_spec.at("typed_io_contract").is_object() &&
         !port_spec.at("typed_io_contract").empty();
}

}  // namespace

RuntimeZeroCopyMode parseRuntimeZeroCopyMode(const std::string& value) {
  const std::string mode = normalizedModeText(value.empty() ? "auto" : value);
  if (mode == "off" || mode == "disabled" || mode == "false" || mode == "0") {
    return RuntimeZeroCopyMode::Off;
  }
  if (mode == "prefer" || mode == "preferred") {
    return RuntimeZeroCopyMode::Prefer;
  }
  if (mode == "require" || mode == "required" || mode == "strict") {
    return RuntimeZeroCopyMode::Require;
  }
  if (mode == "auto" || mode == "default" || mode.empty()) {
    return RuntimeZeroCopyMode::Auto;
  }
  throw std::runtime_error(
      "Unsupported runtime zero-copy mode: " + value +
      " (expected off, prefer, require, or auto)");
}

std::string runtimeZeroCopyModeName(RuntimeZeroCopyMode mode) {
  switch (mode) {
    case RuntimeZeroCopyMode::Off:
      return "off";
    case RuntimeZeroCopyMode::Prefer:
      return "prefer";
    case RuntimeZeroCopyMode::Require:
      return "require";
    case RuntimeZeroCopyMode::Auto:
      return "auto";
  }
  return "auto";
}

RuntimeZeroCopyMode resolveRuntimeZeroCopyMode(const std::string& requested_mode) {
  if (envFlagEnabled("FLIGHTENV_DISABLE_TYPED_ADAPTER_ABI_V2")) {
    return RuntimeZeroCopyMode::Off;
  }
  const std::string env_mode = envString("FLIGHTENV_RUNTIME_ZERO_COPY_MODE");
  if (!env_mode.empty()) {
    return parseRuntimeZeroCopyMode(env_mode);
  }
  return parseRuntimeZeroCopyMode(requested_mode.empty() ? "auto" : requested_mode);
}

bool zeroCopyRequiredForPort(const nlohmann::json& port_spec) {
  if (!port_spec.is_object()) {
    return false;
  }
  const nlohmann::json typed = port_spec.value("typed_io_contract", nlohmann::json::object());
  return typed.is_object() &&
         (jsonBool(typed, "json_operator_io_forbidden", false) ||
          jsonBool(typed, "json_hot_path_forbidden", false));
}

bool dataPlaneHasTypedIoContract(const nlohmann::json& data_plane_info) {
  if (!data_plane_info.is_object()) {
    return false;
  }
  for (const char* section : {"inputs", "outputs"}) {
    const nlohmann::json ports = data_plane_info.value(section, nlohmann::json::array());
    if (!ports.is_array()) {
      continue;
    }
    for (const nlohmann::json& spec : ports) {
      if (portHasTypedContract(spec)) {
        return true;
      }
    }
  }
  return false;
}

bool dataPlaneRequiresTypedExecute(const nlohmann::json& data_plane_info) {
  if (!data_plane_info.is_object()) {
    return false;
  }
  const nlohmann::json outputs = data_plane_info.value("outputs", nlohmann::json::array());
  if (!outputs.is_array()) {
    return false;
  }
  for (const nlohmann::json& spec : outputs) {
    if (zeroCopyRequiredForPort(spec)) {
      return true;
    }
  }
  return false;
}

bool hasZeroCopyPayloadRef(const nlohmann::json& payload) {
  if (!payload.is_object()) {
    return false;
  }
  if (hasObjectRef(payload, "typed_buffer_ref")) return true;
  if (hasObjectRef(payload, "tensor_ref")) return true;
  if (hasObjectRef(payload, "artifact_ref")) return true;
  if (hasObjectRef(payload, "tensor_alignment_ref")) return true;
  if (hasNonEmptyStringRef(payload, "typed_payload_ref")) return true;
  return false;
}

bool allowInlineJsonTypedPayloadBridge() {
  return envFlagEnabled("FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE");
}

RuntimeZeroCopyExecuteDecision decideRuntimeZeroCopyExecute(
    const nlohmann::json& data_plane_info,
    const nlohmann::json& payload,
    const std::string& requested_mode,
    bool in_process_session,
    bool typed_adapter_available,
    const std::string& node_id,
    const std::string& adapter_id) {
  RuntimeZeroCopyExecuteDecision decision;
  decision.mode = resolveRuntimeZeroCopyMode(requested_mode);
  decision.in_process_session = in_process_session;
  decision.typed_adapter_available = typed_adapter_available;
  decision.typed_output_required = dataPlaneRequiresTypedExecute(data_plane_info);
  decision.typed_contract_present = dataPlaneHasTypedIoContract(data_plane_info);
  decision.input_buffer_ref_present = hasZeroCopyPayloadRef(payload);

  const bool can_use_typed =
      decision.in_process_session &&
      decision.typed_adapter_available &&
      (decision.typed_contract_present || decision.input_buffer_ref_present);
  const bool should_try_typed =
      decision.typed_contract_present || decision.input_buffer_ref_present;

  if (decision.mode == RuntimeZeroCopyMode::Off) {
    decision.use_typed_execute = false;
    decision.reason = decision.typed_output_required
                          ? "zero_copy_disabled_default_abi_with_policy_relaxed"
                          : "zero_copy_disabled";
    decision.message =
        "Node " + node_id + " adapter " + adapter_id +
        " uses default ABI because runtime zero-copy mode is off.";
    return decision;
  }

  if (can_use_typed) {
    decision.use_typed_execute = true;
    decision.reason = decision.input_buffer_ref_present
                          ? "in_process_typed_buffer_input"
                          : "in_process_typed_contract";
    return decision;
  }

  if (decision.mode == RuntimeZeroCopyMode::Require ||
      (decision.mode == RuntimeZeroCopyMode::Auto && decision.typed_output_required)) {
    if (should_try_typed || decision.typed_output_required) {
      decision.fail_fast = true;
      decision.reason = !decision.in_process_session
                            ? "not_in_process_session"
                            : (!decision.typed_adapter_available
                                   ? "typed_adapter_abi_unavailable"
                                   : "typed_contract_unavailable");
      decision.message =
          "Node " + node_id + " adapter " + adapter_id +
          " cannot satisfy runtime zero-copy mode " +
          runtimeZeroCopyModeName(decision.mode) +
          ": " + decision.reason +
          ". Use an in-process typed ABI v2 adapter, relax mode to prefer/auto, "
          "or remove typed-only hot-path requirements from the port contract.";
      return decision;
    }
  }

  decision.use_typed_execute = false;
  decision.reason = should_try_typed ? "typed_fast_path_unavailable_default_abi"
                                     : "no_typed_data_plane";
  return decision;
}

nlohmann::json runtimeZeroCopyExecuteDecisionToJson(
    const RuntimeZeroCopyExecuteDecision& decision) {
  return nlohmann::json{
      {"mode", runtimeZeroCopyModeName(decision.mode)},
      {"in_process_session", decision.in_process_session},
      {"typed_adapter_available", decision.typed_adapter_available},
      {"typed_output_required", decision.typed_output_required},
      {"typed_contract_present", decision.typed_contract_present},
      {"input_buffer_ref_present", decision.input_buffer_ref_present},
      {"use_typed_execute", decision.use_typed_execute},
      {"fail_fast", decision.fail_fast},
      {"reason", decision.reason},
      {"message", decision.message}};
}

RuntimeZeroCopyCheck checkOutputZeroCopyPolicy(
    const nlohmann::json& port_spec,
    const nlohmann::json& payload,
    const std::string& node_id,
    const std::string& port_id) {
  RuntimeZeroCopyCheck check;
  check.required = zeroCopyRequiredForPort(port_spec);
  check.has_reference = hasZeroCopyPayloadRef(payload);
  check.compliant = !check.required || check.has_reference;
  if (!check.compliant) {
    check.reason = "typed_only_output_inline_json";
    check.message =
        "Node " + node_id + " output port " + port_id +
        " declares json_operator_io_forbidden=true but returned inline-only JSON. "
        "Use adapter typed ABI v2 with runtime-owned typed_buffer_ref, tensor_ref, or artifact_ref. "
        "Set FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE=1 only for migration diagnostics.";
  }
  return check;
}

void enforceOutputZeroCopyPolicy(
    const nlohmann::json& data_plane_info,
    const nlohmann::json& execute_result,
    const std::string& node_id) {
  const nlohmann::json output_specs = data_plane_info.value("outputs", nlohmann::json::array());
  if (!output_specs.is_array() || output_specs.empty()) {
    return;
  }
  const nlohmann::json outputs = execute_result.value("outputs", nlohmann::json::object());
  for (const auto& spec : output_specs) {
    if (!spec.is_object() || !zeroCopyRequiredForPort(spec)) {
      continue;
    }
    const std::string port_id = jsonString(spec, "port_id");
    if (port_id.empty()) {
      continue;
    }
    const nlohmann::json payload =
        outputs.is_object() && outputs.contains(port_id) ? outputs.at(port_id) : nlohmann::json::object();
    const RuntimeZeroCopyCheck check = checkOutputZeroCopyPolicy(spec, payload, node_id, port_id);
    if (!check.compliant) {
      throw std::runtime_error(check.message);
    }
  }
}

}  // namespace FlightEnvPlatformRuntime
