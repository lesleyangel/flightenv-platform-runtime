#include "FlightEnvPlatformRuntime/RuntimePortPacketWriter.hpp"

#include "FlightEnvPlatform/Runtime/RuntimePacket.hpp"
#include "FlightEnvPlatform/Runtime/ThreadSafePortStore.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>

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
  if (item.is_number_integer()) {
    return item.get<int>();
  }
  if (item.is_number()) {
    return static_cast<int>(item.get<double>());
  }
  return fallback;
}

std::uint64_t jsonUInt64(const nlohmann::json& value, const std::string& key, std::uint64_t fallback = 0) {
  if (!value.is_object() || !value.contains(key)) {
    return fallback;
  }
  const auto& item = value.at(key);
  if (item.is_number_unsigned()) {
    return item.get<std::uint64_t>();
  }
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

flightenv::platform::SimulationTimePoint simulationTimePointFromJson(const nlohmann::json& value) {
  flightenv::platform::SimulationTimePoint point;
  point.run_time_s = jsonDouble(value, "run_time_s", 0.0);
  point.tick_index = jsonInt(value, "tick_index", 0);
  point.source_time_s = jsonDouble(value, "source_time_s", point.run_time_s);
  point.stamp_ns = static_cast<long long>(jsonDouble(value, "stamp_ns", 0.0));
  return point;
}

std::string storageSegment(std::string text) {
  if (text.empty()) {
    return "_";
  }
  std::replace_if(
      text.begin(),
      text.end(),
      [](unsigned char c) {
        return !(std::isalnum(c) || c == '_' || c == '-' || c == '.');
      },
      '_');
  return text;
}

nlohmann::json outputPorts(const nlohmann::json& execute_result) {
  if (!execute_result.is_object()) {
    return nlohmann::json::object();
  }
  const nlohmann::json outputs = execute_result.value("outputs", nlohmann::json::object());
  return outputs.is_object() ? outputs : nlohmann::json::object();
}

nlohmann::json outputPortValue(const nlohmann::json& execute_result, const std::string& port_id) {
  const nlohmann::json outputs = outputPorts(execute_result);
  if (!port_id.empty() && outputs.contains(port_id)) {
    return outputs.at(port_id);
  }
  return nlohmann::json::object();
}

std::string payloadRefFromEntry(const nlohmann::json& entry) {
  return jsonString(
      entry,
      "ref",
      jsonString(entry,
                 "typed_buffer_ref",
                 jsonString(entry,
                            "typed_payload_ref",
                            jsonString(entry, "artifact_uri", jsonString(entry, "evidence_ref")))));
}

flightenv::platform::RuntimePacket buildPortPacket(
    const RuntimePortPacketWriteRequest& request,
    const nlohmann::json& entry) {
  const std::string node_id = request.node_id;
  const std::string port_id = jsonString(entry, "port_id");
  const nlohmann::json output_time =
      request.time_info.value("output_time_point", request.time_info.value("time_point", nlohmann::json::object()));
  const nlohmann::json port_value = outputPortValue(request.execute_result, port_id);
  const std::string payload_ref = payloadRefFromEntry(entry);

  flightenv::platform::RuntimePacket packet;
  packet.run_id = request.run_id;
  packet.object_id = request.object_id;
  packet.port_name = "node." + storageSegment(node_id) + ".port." + storageSegment(port_id);
  packet.node_id = node_id;
  packet.time_point = simulationTimePointFromJson(output_time);
  packet.producer_node = node_id;
  packet.payload_kind = "port_ref_json";
  packet.payload_ref = payload_ref;
  packet.contract_id = jsonString(entry, "contract_id");
  packet.typed_schema_id = jsonString(entry, "typed_schema_id");
  packet.typed_dto_name = jsonString(entry, "typed_dto_name");
  packet.typed_payload_ref = jsonString(entry, "typed_payload_ref");
  packet.typed_buffer_ref = jsonString(entry, "typed_buffer_ref");
  packet.buffer_layout_id = jsonString(entry, "buffer_layout_id", jsonString(entry, "layout_ref"));
  packet.buffer_bytes = jsonUInt64(entry, "buffer_bytes", 0);
  packet.zero_copy_eligible = jsonBool(entry, "zero_copy_eligible", false);
  packet.checksum = jsonString(entry, "checksum");
  packet.inline_payload_json = nlohmann::json{
      {"summary_kind", "port_ref"},
      {"status", jsonString(request.execute_result, "status", "ok")},
      {"branch_id", request.branch_id},
      {"timeline_id", request.timeline_id},
      {"port_id", port_id},
      {"output_port", port_value},
      {"data_plane_ref", entry},
  }.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  packet.tags["created_at_utc"] = nowUtcIso();
  packet.tags["branch_id"] = request.branch_id;
  packet.tags["timeline_id"] = request.timeline_id;
  packet.tags["direction"] = "output";
  packet.tags["port_id"] = port_id;
  packet.tags["representation"] = jsonString(entry, "representation", packet.payload_kind);
  packet.tags["role"] = jsonString(entry, "role");
  packet.tags["value_kind"] = jsonString(entry, "value_kind");
  return packet;
}

nlohmann::json packetToJson(const flightenv::platform::RuntimePacket& packet) {
  nlohmann::json payload = nlohmann::json::object();
  try {
    payload = nlohmann::json::parse(packet.inline_payload_json);
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

}  // namespace

RuntimePortPacketWriteResult RuntimePortPacketWriter::writeOutputPortPackets(
    flightenv::platform::ThreadSafePortStore& port_store,
    const RuntimePortPacketWriteRequest& request) {
  RuntimePortPacketWriteResult result;
  if (!request.data_plane_entries.is_array()) {
    return result;
  }

  for (const auto& entry : request.data_plane_entries) {
    if (!entry.is_object() || jsonString(entry, "direction") != "output") {
      continue;
    }
    const std::string port_id = jsonString(entry, "port_id");
    if (port_id.empty()) {
      continue;
    }
    const flightenv::platform::RuntimePacket stored = port_store.write(buildPortPacket(request, entry));
    const nlohmann::json summary = packetToJson(stored);
    result.packets.push_back(summary);
    result.packet_by_port[port_id] = summary;
  }

  return result;
}

}  // namespace FlightEnvPlatformRuntime
