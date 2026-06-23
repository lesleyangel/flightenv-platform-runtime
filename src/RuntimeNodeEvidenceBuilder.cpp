#include "FlightEnvPlatformRuntime/RuntimeNodeEvidenceBuilder.hpp"

namespace FlightEnvPlatformRuntime {
namespace {

std::string jsonString(
    const nlohmann::json& value,
    const std::string& key,
    const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

}  // namespace

nlohmann::json RuntimeNodeEvidenceBuilder::artifactSummary(
    const std::string& node_id,
    int loop_iteration_index,
    const std::string& artifact_id,
    const std::string& kind,
    const nlohmann::json& summary) {
  return {
      {"node_id", node_id},
      {"loop_iteration_index", loop_iteration_index},
      {"artifact_id", artifact_id},
      {"kind", kind},
      {"checksum", summary.value("digest", "")},
      {"summary", summary},
  };
}

RuntimeNodeEvidenceResult RuntimeNodeEvidenceBuilder::build(
    const RuntimeNodeEvidenceRequest& request) {
  RuntimeNodeEvidenceResult result;

  result.input_artifact = artifactSummary(
      request.node_id,
      request.loop_iteration_index,
      request.execution_tag + ".input",
      "runtime_input_summary",
      request.input_summary);

  result.output_artifact = artifactSummary(
      request.node_id,
      request.loop_iteration_index,
      request.execution_tag + ".output",
      "runtime_output_summary",
      request.output_summary);

  result.node_snapshot = {
      {"node_id", request.node_id},
      {"operator_id", request.operator_id},
      {"adapter_id", request.adapter_id},
      {"execution_kind", request.execution_kind},
      {"adapter_protocol", request.adapter_protocol},
      {"status", "ok"},
      {"loop_iteration_index", request.loop_iteration_index},
      {"time_info", request.time_info},
      {"adapter_snapshot", request.adapter_snapshot},
      {"runtime_packet", request.runtime_packet},
      {"runtime_port_packets", request.runtime_port_packets},
  };

  result.uncertainty_node = {
      {"node_id", request.node_id},
      {"loop_iteration_index", request.loop_iteration_index},
      {"operator_id", request.operator_id},
      {"uncertainty_contract", request.uncertainty_contract},
      {"input_artifact_hashes", nlohmann::json::array({request.input_summary.value("digest", "")})},
      {"output_artifact_hashes", nlohmann::json::array({request.output_summary.value("digest", "")})},
  };

  result.checkpoint = {
      {"checkpoint_id", request.execution_tag},
      {"node_id", request.node_id},
      {"loop_iteration_index", request.loop_iteration_index},
      {"operator_id", request.operator_id},
      {"checkpoint_kind", request.checkpoint_kind.empty() ? "snapshot_only" : request.checkpoint_kind},
      {"replay_mode", request.replay_mode.empty() ? "record_replay" : request.replay_mode},
      {"time_point", request.time_info.value("time_point", nlohmann::json::object())},
      {"adapter_protocol", request.adapter_protocol},
      {"adapter_snapshot", request.adapter_snapshot},
      {"input_hashes", nlohmann::json::array({request.input_summary.value("digest", "")})},
      {"output_hashes", nlohmann::json::array({request.output_summary.value("digest", "")})},
  };

  // Preserve a small amount of defensive compatibility for older evidence
  // readers that looked for packet refs directly under checkpoint metadata.
  const std::string packet_ref = jsonString(request.runtime_packet, "payload_ref");
  if (!packet_ref.empty()) {
    result.checkpoint["runtime_packet_ref"] = packet_ref;
  }

  return result;
}

}  // namespace FlightEnvPlatformRuntime
