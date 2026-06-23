#include "FlightEnvPlatformRuntime/RuntimePortStoreView.hpp"

#include "FlightEnvPlatform/Runtime/ThreadSafePortStore.hpp"

#include <cstdint>

namespace FlightEnvPlatformRuntime {
namespace {

std::string jsonString(const nlohmann::json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key)) {
    return fallback;
  }
  const auto& item = value.at(key);
  if (item.is_string()) {
    return item.get<std::string>();
  }
  return fallback;
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

nlohmann::json parsePayload(const std::string& text) {
  if (text.empty()) {
    return nlohmann::json::object();
  }
  try {
    return nlohmann::json::parse(text);
  } catch (...) {
    return {{"raw", text}};
  }
}

nlohmann::json packetSummary(const flightenv::platform::RuntimePacket& packet) {
  return {
      {"port_name", packet.port_name},
      {"node_id", packet.node_id},
      {"branch_id", packet.tags.count("branch_id") ? packet.tags.at("branch_id") : ""},
      {"timeline_id", packet.tags.count("timeline_id") ? packet.tags.at("timeline_id") : ""},
      {"producer_node", packet.producer_node},
      {"version", packet.version},
      {"payload_kind", packet.payload_kind},
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
      {"time_s", packet.time_point.run_time_s},
      {"tick_index", packet.time_point.tick_index},
      {"source_time_s", packet.time_point.source_time_s},
      {"stamp_ns", packet.time_point.stamp_ns},
  };
}

nlohmann::json refFromEntry(
    const nlohmann::json& entry,
    const flightenv::platform::RuntimePacket& packet) {
  const std::string representation = jsonString(entry, "representation", packet.payload_kind);
  const std::string ref = jsonString(
      entry,
      "ref",
      jsonString(entry,
                 "typed_buffer_ref",
                 jsonString(entry,
                            "typed_payload_ref",
                            jsonString(entry, "artifact_uri", packet.payload_ref))));
  return {
      {"source", "ThreadSafePortStore"},
      {"has_carried_value", true},
      {"representation", representation.empty() ? "runtime_packet" : representation},
      {"ref", ref},
      {"packet_port_name", packet.port_name},
      {"packet_version", packet.version},
      {"packet_payload_kind", packet.payload_kind},
      {"contract_id", jsonString(entry, "contract_id", packet.contract_id)},
      {"typed_schema_id", jsonString(entry, "typed_schema_id", packet.typed_schema_id)},
      {"typed_dto_name", jsonString(entry, "typed_dto_name", packet.typed_dto_name)},
      {"typed_buffer_ref", jsonString(entry, "typed_buffer_ref", packet.typed_buffer_ref)},
      {"typed_payload_ref", jsonString(entry, "typed_payload_ref", packet.typed_payload_ref)},
      {"buffer_layout_id", jsonString(entry, "buffer_layout_id", packet.buffer_layout_id)},
      {"buffer_bytes", jsonUInt64(entry, "buffer_bytes", packet.buffer_bytes)},
      {"zero_copy_eligible", jsonBool(entry, "zero_copy_eligible", packet.zero_copy_eligible)},
      {"value_kind", jsonString(entry, "value_kind")},
      {"role", jsonString(entry, "role")},
      {"time_point", entry.value("time_point", nlohmann::json::object())},
      {"time_summary", entry.value("time_summary", nlohmann::json::object())},
      {"statistics", entry.value("statistics", nlohmann::json::object())},
  };
}

nlohmann::json refFromPortValue(
    const nlohmann::json& value,
    const flightenv::platform::RuntimePacket& packet) {
  return {
      {"source", "ThreadSafePortStore"},
      {"has_carried_value", value.is_object()},
      {"representation", jsonString(value, "representation", packet.payload_kind)},
      {"ref", jsonString(value, "ref", packet.payload_ref)},
      {"packet_port_name", packet.port_name},
      {"packet_version", packet.version},
      {"packet_payload_kind", packet.payload_kind},
      {"contract_id", jsonString(value, "contract_id", packet.contract_id)},
      {"typed_schema_id", jsonString(value, "typed_schema_id", packet.typed_schema_id)},
      {"typed_dto_name", jsonString(value, "typed_dto_name", packet.typed_dto_name)},
      {"typed_buffer_ref", jsonString(value, "typed_buffer_ref", packet.typed_buffer_ref)},
      {"typed_payload_ref", jsonString(value, "typed_payload_ref", packet.typed_payload_ref)},
      {"buffer_layout_id", jsonString(value, "buffer_layout_id", packet.buffer_layout_id)},
      {"buffer_bytes", packet.buffer_bytes},
      {"zero_copy_eligible", packet.zero_copy_eligible},
      {"value_kind", jsonString(value, "value_kind")},
      {"role", jsonString(value, "role", jsonString(value, "display_role"))},
      {"time_point", value.value("time_point", nlohmann::json::object())},
      {"statistics", value.value("statistics", nlohmann::json::object())},
  };
}

nlohmann::json refFromPortPacket(
    const flightenv::platform::RuntimePacket& packet,
    const std::string& port_id) {
  const nlohmann::json payload = parsePayload(packet.inline_payload_json);
  const nlohmann::json entry = payload.value("data_plane_ref", nlohmann::json::object());
  nlohmann::json carried = nlohmann::json::object();
  if (entry.is_object() && !entry.empty()) {
    carried = refFromEntry(entry, packet);
  } else {
    carried = refFromPortValue(payload.value("output_port", nlohmann::json::object()), packet);
  }
  carried["source"] = "ThreadSafePortStore.port_packet";
  carried["is_port_packet"] = true;
  carried["port_id"] = port_id;
  carried["packet"] = packetSummary(packet);
  return carried;
}

}  // namespace

nlohmann::json RuntimePortStoreView::nodeOutputRefs(
    const flightenv::platform::ThreadSafePortStore& port_store,
    const std::string& node_id,
    const nlohmann::json& data_plane_info) {
  nlohmann::json view = {
      {"source", "ThreadSafePortStore"},
      {"node_id", node_id},
      {"packet_found", false},
      {"ports", nlohmann::json::object()},
  };

  const auto packet_opt = port_store.read_node_output(node_id);
  const flightenv::platform::RuntimePacket* packet = packet_opt.has_value() ? &packet_opt.value() : nullptr;
  if (packet != nullptr) {
    view["packet_found"] = true;
    view["packet"] = packetSummary(*packet);
  }

  const nlohmann::json payload = packet != nullptr ? parsePayload(packet->inline_payload_json) : nlohmann::json::object();
  const nlohmann::json output_ports = payload.value("output_ports", nlohmann::json::object());
  const nlohmann::json data_plane_refs = payload.value("data_plane_refs", nlohmann::json::array());
  const nlohmann::json output_specs = data_plane_info.value("outputs", nlohmann::json::array());

  if (output_specs.is_array()) {
    for (const auto& spec : output_specs) {
      if (!spec.is_object()) {
        continue;
      }
      const std::string port_id = jsonString(spec, "port_id");
      if (port_id.empty()) {
        continue;
      }
      const auto port_packet_opt = port_store.read_node_port_output(node_id, port_id);
      nlohmann::json carried = nlohmann::json::object();
      if (port_packet_opt.has_value()) {
        carried = refFromPortPacket(port_packet_opt.value(), port_id);
        view["packet_found"] = true;
      }
      if (carried.empty() && data_plane_refs.is_array() && packet != nullptr) {
        for (const auto& entry : data_plane_refs) {
          if (entry.is_object() && jsonString(entry, "direction") == "output" &&
              jsonString(entry, "port_id") == port_id) {
            carried = refFromEntry(entry, *packet);
            break;
          }
        }
      }
      if (carried.empty() && output_ports.is_object() && output_ports.contains(port_id) && packet != nullptr) {
        carried = refFromPortValue(output_ports.at(port_id), *packet);
      }
      if (carried.empty()) {
        carried = {
            {"source", "ThreadSafePortStore"},
            {"has_carried_value", false},
            {"representation", "none"},
            {"reason", "port_not_found_in_packet"},
            {"packet_port_name", packet != nullptr ? packet->port_name : ""},
            {"packet_version", packet != nullptr ? packet->version : 0},
      };
      }
      carried["port_id"] = port_id;
      view["ports"][port_id] = carried;
    }
  }

  return view;
}

}  // namespace FlightEnvPlatformRuntime
