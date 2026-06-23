#include "FlightEnvPlatformRuntime/RuntimeAdapterInvoker.hpp"

#include "FlightEnvPlatformRuntime/RuntimeTypedPayloadBridge.hpp"
#include "FlightEnvPlatformRuntime/RuntimeZeroCopyPolicy.hpp"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace FlightEnvPlatformRuntime {
namespace {

std::string jsonString(const nlohmann::json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

int jsonInt(const nlohmann::json& value, const std::string& key, int fallback = 0) {
  if (!value.is_object() || !value.contains(key)) {
    return fallback;
  }
  const auto& item = value.at(key);
  if (item.is_number_integer()) {
    return item.get<int>();
  }
  if (item.is_number()) {
    return static_cast<int>(item.get<double>());
  }
  return fallback;
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
  return {
      {"type", value.type_name()},
      {"byte_size", static_cast<int>(text.size())},
      {"digest_algorithm", "fnv64"},
      {"digest", digestJson(value)},
  };
}

bool hasRuntimePayloadReference(const nlohmann::json& value) {
  if (!value.is_object()) {
    return false;
  }
  if (hasZeroCopyPayloadRef(value)) {
    return true;
  }
  for (const auto* key : {"artifact_uri", "artifact_path", "uri", "path"}) {
    if (value.contains(key) && value.at(key).is_string() && !value.at(key).get<std::string>().empty()) {
      return true;
    }
  }
  return false;
}

void trace(const RuntimeAdapterInvokeRequest& request, const std::string& message) {
  if (request.trace) {
    request.trace(message);
  }
}

}  // namespace

nlohmann::json RuntimeAdapterInvoker::execute(
    IAdapterSession& session,
    const RuntimeAdapterInvokeRequest& request) {
  nlohmann::json execute_result =
      session.execute(request.context, {{"upstream", request.upstream}, {"data_plane", request.data_plane_info}});

  trace(request, "execute_node_typed_materialize_begin iteration=" +
                     std::to_string(request.iteration_index) + " node=" + request.node_id);
  execute_result = materializeTypedOutputPayloads(
      request.run_dir,
      request.data_plane_info,
      execute_result,
      request.time_info,
      request.iteration_index,
      request.typed_buffer_persistence);
  trace(request, "execute_node_typed_materialize_end iteration=" +
                     std::to_string(request.iteration_index) + " node=" + request.node_id);

  const RuntimeZeroCopyMode zero_copy_mode = resolveRuntimeZeroCopyMode(request.runtime_zero_copy_mode);
  if (zero_copy_mode != RuntimeZeroCopyMode::Off) {
    enforceOutputZeroCopyPolicy(request.data_plane_info, execute_result, request.node_id);
    trace(request, "execute_node_zero_copy_policy_end iteration=" +
                       std::to_string(request.iteration_index) + " node=" + request.node_id);
  } else {
    trace(request, "execute_node_zero_copy_policy_skipped mode=off iteration=" +
                       std::to_string(request.iteration_index) + " node=" + request.node_id);
  }

  validateOutputContracts(
      request.data_plane_info,
      execute_result,
      request.node_id,
      zero_copy_mode == RuntimeZeroCopyMode::Off);
  trace(request, "execute_node_validate_contracts_end iteration=" +
                     std::to_string(request.iteration_index) + " node=" + request.node_id);
  return execute_result;
}

void RuntimeAdapterInvoker::validateOutputContracts(
    const nlohmann::json& data_plane_info,
    const nlohmann::json& execute_result,
    const std::string& node_id,
    bool allow_inline_typed_outputs) {
  const nlohmann::json output_specs = data_plane_info.value("outputs", nlohmann::json::array());
  if (!output_specs.is_array() || output_specs.empty()) {
    return;
  }
  if (!execute_result.is_object() || !execute_result.contains("outputs") ||
      !execute_result.at("outputs").is_object()) {
    throw std::runtime_error("Node " + node_id + " execute result must contain an object 'outputs'.");
  }
  const nlohmann::json& outputs = execute_result.at("outputs");
  for (const auto& spec : output_specs) {
    if (!spec.is_object()) {
      continue;
    }
    const std::string port_id = jsonString(spec, "port_id");
    if (port_id.empty()) {
      continue;
    }
    if (!outputs.contains(port_id)) {
      if (!spec.value("required", true)) {
        continue;
      }
      throw std::runtime_error("Node " + node_id + " missing required output port: " + port_id);
    }
    const nlohmann::json& payload = outputs.at(port_id);
    if (!payload.is_object()) {
      throw std::runtime_error("Node " + node_id + " output port " + port_id + " must be an object.");
    }
    for (const auto* key : {"contract_id", "frame_contract", "value_kind"}) {
      const std::string expected = jsonString(spec, key);
      if (payload.contains(key) && !expected.empty() && jsonString(payload, key) != expected) {
        throw std::runtime_error(
            "Node " + node_id + " output port " + port_id + " " + key +
            " mismatch: expected '" + expected + "', got '" + jsonString(payload, key) + "'.");
      }
    }
    const nlohmann::json policy = spec.value("data_policy", nlohmann::json::object());
    const bool requires_artifact = policy.value("artifact_required", false);
    const bool requires_tensor = policy.value("tensor_required", false);
    const bool requires_typed_ref = typedIoJsonForbidden(spec) && !allow_inline_typed_outputs;
    if ((requires_artifact || requires_tensor || requires_typed_ref) && !hasRuntimePayloadReference(payload)) {
      throw std::runtime_error(
          "Node " + node_id + " output port " + port_id +
          " requires artifact_ref/tensor_ref/typed_payload_ref/typed_buffer_ref; inline-only output is not allowed.");
    }
    const int max_inline_bytes = jsonInt(policy, "max_inline_bytes", 0);
    if (max_inline_bytes > 0 && !hasRuntimePayloadReference(payload)) {
      const int byte_size = jsonInt(summarizeJson(payload), "byte_size", 0);
      if (byte_size > max_inline_bytes) {
        throw std::runtime_error(
            "Node " + node_id + " output port " + port_id +
            " inline payload exceeds max_inline_bytes=" + std::to_string(max_inline_bytes));
      }
    }
  }
}

}  // namespace FlightEnvPlatformRuntime
