#include "NativeWorkflowNodeResultCommit.hpp"

#include "FlightEnvPlatform/Runtime/ThreadSafePortStore.hpp"
#include "FlightEnvPlatformRuntime/RuntimePortPacketWriter.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeMaterialization.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstdint>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

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

std::string jsonString(const json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

int jsonInt(const json& value, const std::string& key, int fallback = 0) {
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

double jsonDouble(const json& value, const std::string& key, double fallback = 0.0) {
  if (!value.is_object() || !value.contains(key)) {
    return fallback;
  }
  const auto& item = value.at(key);
  if (item.is_number()) {
    return item.get<double>();
  }
  return fallback;
}

std::uint64_t jsonUInt64(const json& value, const std::string& key, std::uint64_t fallback = 0) {
  if (!value.is_object() || !value.contains(key)) {
    return fallback;
  }
  const auto& item = value.at(key);
  if (item.is_number_unsigned()) {
    return item.get<std::uint64_t>();
  }
  if (item.is_number_integer()) {
    const auto signed_value = item.get<long long>();
    return signed_value < 0 ? fallback : static_cast<std::uint64_t>(signed_value);
  }
  if (item.is_number_float()) {
    const double number = item.get<double>();
    return number < 0.0 ? fallback : static_cast<std::uint64_t>(number);
  }
  return fallback;
}

bool jsonBool(const json& value, const std::string& key, bool fallback = false) {
  if (!value.is_object() || !value.contains(key)) {
    return fallback;
  }
  const auto& item = value.at(key);
  if (item.is_boolean()) {
    return item.get<bool>();
  }
  if (item.is_number_integer()) {
    return item.get<int>() != 0;
  }
  if (item.is_string()) {
    std::string text = item.get<std::string>();
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return text == "1" || text == "true" || text == "yes" || text == "on";
  }
  return fallback;
}

json summarizeJson(const json& value) {
  if (!value.is_object()) {
    return {{"type", value.type_name()}};
  }
  json summary = json::object();
  for (auto it = value.begin(); it != value.end(); ++it) {
    if (it->is_array()) {
      summary[it.key()] = {{"type", "array"}, {"size", it->size()}};
    } else if (it->is_object()) {
      summary[it.key()] = {{"type", "object"}, {"size", it->size()}};
    } else {
      summary[it.key()] = *it;
    }
  }
  return summary;
}

flightenv::platform::SimulationTimePoint simulationTimePointFromJson(const json& value) {
  flightenv::platform::SimulationTimePoint point;
  point.run_time_s = jsonDouble(value, "run_time_s", 0.0);
  point.tick_index = jsonInt(value, "tick_index", 0);
  point.source_time_s = jsonDouble(value, "source_time_s", point.run_time_s);
  point.stamp_ns = static_cast<long long>(jsonDouble(value, "stamp_ns", 0.0));
  return point;
}

flightenv::platform::RuntimePacket buildRuntimePacket(
    const NativeWorkflowNodeResultCommitRequest& request,
    const json& data_plane_entries) {
  const json output_time =
      request.time_info.value("output_time_point", request.time_info.value("time_point", json::object()));
  json output_refs = json::array();
  json first_output_ref = json::object();
  if (data_plane_entries.is_array()) {
    for (const auto& entry : data_plane_entries) {
      if (!entry.is_object() || jsonString(entry, "direction") != "output") {
        continue;
      }
      output_refs.push_back(entry);
      if (first_output_ref.empty()) {
        first_output_ref = entry;
      }
    }
  }

  flightenv::platform::RuntimePacket packet;
  packet.run_id = request.run_id;
  packet.object_id = request.object_id;
  packet.port_name = "node." + request.node_id + ".output";
  packet.node_id = request.node_id;
  packet.time_point = simulationTimePointFromJson(output_time);
  packet.producer_node = request.node_id;
  packet.payload_kind = "inline_summary_json";
  packet.inline_payload_json = json{
      {"status", jsonString(request.execute_result, "status", "ok")},
      {"output_ports", request.execute_result.value("outputs", json::object())},
      {"data_plane_refs", output_refs},
      {"summary", summarizeJson(request.execute_result)},
  }.dump(-1, ' ', false, json::error_handler_t::replace);
  packet.payload_ref = "runtime_node_snapshot.json";
  if (first_output_ref.is_object()) {
    packet.contract_id = jsonString(first_output_ref, "contract_id");
    packet.typed_schema_id = jsonString(first_output_ref, "typed_schema_id");
    packet.typed_dto_name = jsonString(first_output_ref, "typed_dto_name");
    packet.typed_payload_ref = jsonString(first_output_ref, "typed_payload_ref");
    packet.typed_buffer_ref = jsonString(first_output_ref, "typed_buffer_ref");
    packet.buffer_layout_id =
        jsonString(first_output_ref, "buffer_layout_id", jsonString(first_output_ref, "layout_ref"));
    packet.buffer_bytes = jsonUInt64(first_output_ref, "buffer_bytes", 0);
    packet.zero_copy_eligible = jsonBool(first_output_ref, "zero_copy_eligible", false);
  }
  packet.tags["created_at_utc"] = nowUtcIso();
  packet.tags["branch_id"] = request.branch_id;
  packet.tags["timeline_id"] = request.timeline_id;
  packet.tags["data_plane_ref_count"] = std::to_string(output_refs.size());
  return packet;
}

void trace(const NativeWorkflowNodeResultCommitRequest& request, const std::string& message) {
  if (request.trace) {
    request.trace(message);
  }
}

}  // namespace

json nativeWorkflowRuntimePacketToJson(const flightenv::platform::RuntimePacket& packet) {
  json payload = json::object();
  try {
    payload = json::parse(packet.inline_payload_json);
  } catch (...) {
    payload = {{"raw", packet.inline_payload_json}};
  }
  return {
      {"run_id", packet.run_id},
      {"object_id", packet.object_id},
      {"branch_id", packet.tags.count("branch_id") ? packet.tags.at("branch_id") : ""},
      {"timeline_id", packet.tags.count("timeline_id") ? packet.tags.at("timeline_id") : ""},
      {"port_name", packet.port_name},
      {"node_id", packet.node_id},
      {"time_s", packet.time_point.run_time_s},
      {"tick_index", packet.time_point.tick_index},
      {"source_time_s", packet.time_point.source_time_s},
      {"stamp_ns", packet.time_point.stamp_ns},
      {"version", packet.version},
      {"producer_node", packet.producer_node},
      {"payload_kind", packet.payload_kind},
      {"payload", payload},
      {"payload_ref", packet.payload_ref},
      {"contract_id", packet.contract_id},
      {"typed_schema_id", packet.typed_schema_id},
      {"typed_dto_name", packet.typed_dto_name},
      {"typed_payload_ref", packet.typed_payload_ref},
      {"typed_buffer_ref", packet.typed_buffer_ref},
      {"buffer_layout_id", packet.buffer_layout_id},
      {"buffer_bytes", packet.buffer_bytes},
      {"zero_copy_eligible", packet.zero_copy_eligible},
      {"checksum", packet.checksum},
      {"tags", packet.tags},
      {"created_at_utc", packet.tags.count("created_at_utc") ? packet.tags.at("created_at_utc") : nowUtcIso()},
  };
}

NativeWorkflowNodeResultCommitResult commitNativeWorkflowNodeResult(
    const NativeWorkflowNodeResultCommitRequest& request) {
  if (request.port_store == nullptr) {
    throw std::runtime_error("NativeWorkflowNodeResultCommit requires a non-null port_store.");
  }

  NativeWorkflowNodeResultCommitResult result;

  trace(request, "execute_node_dataplane_begin iteration=" + std::to_string(request.iteration_index) +
                     " node=" + request.node_id);
  result.data_plane_entries = RuntimeMaterialization::makeDataPlaneEntries(
      request.data_plane_info,
      request.upstream,
      request.execute_result,
      request.time_info,
      request.iteration_index);
  trace(request, "execute_node_dataplane_end iteration=" + std::to_string(request.iteration_index) +
                     " node=" + request.node_id);

  trace(request, "execute_node_packet_build_begin iteration=" + std::to_string(request.iteration_index) +
                     " node=" + request.node_id);
  flightenv::platform::RuntimePacket packet_candidate = buildRuntimePacket(request, result.data_plane_entries);
  trace(request, "execute_node_packet_build_end iteration=" + std::to_string(request.iteration_index) +
                     " node=" + request.node_id +
                     " payload_bytes=" + std::to_string(packet_candidate.inline_payload_json.size()));

  trace(request, "execute_node_portstore_write_begin iteration=" + std::to_string(request.iteration_index) +
                     " node=" + request.node_id);
  const flightenv::platform::RuntimePacket runtime_packet = request.port_store->write(packet_candidate);
  trace(request, "execute_node_portstore_write_end iteration=" + std::to_string(request.iteration_index) +
                     " node=" + request.node_id);

  trace(request, "execute_node_packet_json_begin iteration=" + std::to_string(request.iteration_index) +
                     " node=" + request.node_id);
  result.runtime_packet = nativeWorkflowRuntimePacketToJson(runtime_packet);
  trace(request, "execute_node_packet_json_end iteration=" + std::to_string(request.iteration_index) +
                     " node=" + request.node_id);

  trace(request, "execute_node_port_packets_begin iteration=" + std::to_string(request.iteration_index) +
                     " node=" + request.node_id);
  const RuntimePortPacketWriteResult port_packet_result =
      RuntimePortPacketWriter::writeOutputPortPackets(
          *request.port_store,
          RuntimePortPacketWriteRequest{
              request.run_id,
              request.object_id,
              request.branch_id,
              request.timeline_id,
              request.node_id,
              request.execute_result,
              request.time_info,
              result.data_plane_entries,
          });
  trace(request, "execute_node_port_packets_end iteration=" + std::to_string(request.iteration_index) +
                     " node=" + request.node_id +
                     " count=" + std::to_string(port_packet_result.packets.size()));
  result.runtime_port_packets = port_packet_result.packets;
  result.runtime_port_packet_by_port = port_packet_result.packet_by_port;

  result.node_evidence = RuntimeNodeEvidenceBuilder::build({
      request.node_id,
      request.operator_id,
      request.adapter_id,
      request.execution_kind,
      request.adapter_protocol,
      request.execution_tag,
      request.iteration_index,
      request.time_info,
      request.adapter_snapshot,
      result.runtime_packet,
      result.runtime_port_packets,
      request.uncertainty_info.value("uncertainty_contract", json::object()),
      request.input_summary,
      request.output_summary,
      jsonString(request.state_store_info, "checkpoint_kind", "snapshot_only"),
      jsonString(request.state_store_info, "replay_mode", "record_replay"),
  });
  return result;
}

}  // namespace FlightEnvPlatformRuntime
