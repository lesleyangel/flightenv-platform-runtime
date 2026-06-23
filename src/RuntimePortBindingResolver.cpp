#include "FlightEnvPlatformRuntime/RuntimePortBindingResolver.hpp"

#include "FlightEnvPlatform/Runtime/RuntimePacket.hpp"
#include "FlightEnvPlatform/Runtime/ThreadSafePortStore.hpp"

#include <cstdint>
#include <string>

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

nlohmann::json ensureObject(nlohmann::json value) {
  return value.is_object() ? std::move(value) : nlohmann::json::object();
}

void ensureBindingContainers(nlohmann::json& upstream) {
  if (!upstream.contains("input_ports") || !upstream.at("input_ports").is_object()) {
    upstream["input_ports"] = nlohmann::json::object();
  }
  if (!upstream.contains("edge_bindings") || !upstream.at("edge_bindings").is_array()) {
    upstream["edge_bindings"] = nlohmann::json::array();
  }
}

nlohmann::json outputPorts(const nlohmann::json& node_output) {
  if (!node_output.is_object()) {
    return nlohmann::json::object();
  }
  const nlohmann::json ports =
      node_output.value("outputs", node_output.value("output_ports", nlohmann::json::object()));
  return ports.is_object() ? ports : nlohmann::json::object();
}

nlohmann::json valueFromRuntimePortPacket(const flightenv::platform::RuntimePacket& packet) {
  nlohmann::json payload = nlohmann::json::object();
  try {
    payload = nlohmann::json::parse(packet.inline_payload_json);
  } catch (...) {
    payload = nlohmann::json::object();
  }
  if (payload.is_object()) {
    const nlohmann::json output_port = payload.value("output_port", nlohmann::json::object());
    if (!output_port.empty()) {
      return output_port;
    }
    const nlohmann::json data_plane_ref = payload.value("data_plane_ref", nlohmann::json::object());
    if (!data_plane_ref.empty()) {
      return data_plane_ref;
    }
  }
  return {
      {"representation", packet.payload_kind},
      {"ref", packet.payload_ref},
      {"contract_id", packet.contract_id},
      {"typed_schema_id", packet.typed_schema_id},
      {"typed_dto_name", packet.typed_dto_name},
      {"typed_payload_ref", packet.typed_payload_ref},
      {"typed_buffer_ref", packet.typed_buffer_ref},
      {"buffer_layout_id", packet.buffer_layout_id},
      {"buffer_bytes", packet.buffer_bytes},
      {"zero_copy_eligible", packet.zero_copy_eligible},
      {"packet_port_name", packet.port_name},
      {"packet_version", packet.version},
  };
}

std::string normalizedPortToken(std::string port_id) {
  const std::string prefix = "field.";
  const std::size_t pos = port_id.find(prefix);
  if (pos != std::string::npos) {
    port_id = port_id.substr(pos + prefix.size());
  }
  for (const std::string suffix : {".next", ".current"}) {
    if (port_id.size() > suffix.size() &&
        port_id.compare(port_id.size() - suffix.size(), suffix.size(), suffix) == 0) {
      port_id = port_id.substr(0, port_id.size() - suffix.size());
    }
  }
  return port_id;
}

void trace(const RuntimePortBindingResolveRequest& request, const std::string& message) {
  if (request.trace) {
    request.trace(message);
  }
}

nlohmann::json bindingEvidenceBase(
    const std::string& target_node_id,
    const std::string& target_port_id,
    const std::string& source_node_id,
    const std::string& source_port_id,
    const std::string& source) {
  return {
      {"target_node_id", target_node_id},
      {"target_port_id", target_port_id},
      {"source_node_id", source_node_id},
      {"source_port_id", source_port_id},
      {"binding_source", source},
  };
}

}  // namespace

RuntimePortBindingResolveResult RuntimePortBindingResolver::resolve(
    const RuntimePortBindingResolveRequest& request) {
  RuntimePortBindingResolveResult result;
  result.upstream = ensureObject(request.base_upstream);
  if (request.external_seed.is_object()) {
    result.upstream.update(request.external_seed);
  }

  const std::string node_id = jsonString(request.node, "node_id");
  result.evidence = {
      {"node_id", node_id},
      {"branch_id", request.branch_id},
      {"timeline_id", request.timeline_id},
      {"event_time_ns", request.event_time_ns},
      {"direct_dependency_count", 0},
      {"explicit_binding_count", 0},
      {"implicit_binding_count", 0},
      {"missing_explicit_bindings", nlohmann::json::array()},
      {"bindings", nlohmann::json::array()},
  };

  if (request.node.contains("depends_on") && request.node.at("depends_on").is_array()) {
    for (const auto& dep : request.node.at("depends_on")) {
      if (!dep.is_string()) {
        continue;
      }
      const std::string source_node_id = dep.get<std::string>();
      if (request.current_outputs.is_object() && request.current_outputs.contains(source_node_id)) {
        result.upstream[source_node_id] = request.current_outputs.at(source_node_id);
        result.evidence["direct_dependency_count"] =
            result.evidence.value("direct_dependency_count", 0) + 1;
      }
    }
  }

  if (request.node.contains("edge_bindings") && request.node.at("edge_bindings").is_array()) {
    ensureBindingContainers(result.upstream);
    for (const auto& binding : request.node.at("edge_bindings")) {
      if (!binding.is_object()) {
        continue;
      }
      const std::string source_node_id = jsonString(binding, "source_node_id");
      const std::string source_port_id = jsonString(binding, "source_port_id");
      const std::string target_port_id = jsonString(binding, "target_port_id");
      if (source_node_id.empty() || source_port_id.empty() || target_port_id.empty()) {
        continue;
      }

      nlohmann::json bound_value = nlohmann::json::object();
      std::string binding_source = "missing";
      if (request.current_outputs.is_object() && request.current_outputs.contains(source_node_id)) {
        const nlohmann::json source_ports = outputPorts(request.current_outputs.at(source_node_id));
        if (source_ports.contains(source_port_id)) {
          bound_value = source_ports.at(source_port_id);
          binding_source = "current_outputs";
        }
      }
      if (bound_value.empty() && request.port_store != nullptr) {
        const auto scoped_packet =
            request.port_store->read_node_port_output_at_or_before(
                request.branch_id,
                request.timeline_id,
                source_node_id,
                source_port_id,
                request.event_time_ns);
        if (scoped_packet.has_value()) {
          bound_value = valueFromRuntimePortPacket(scoped_packet.value());
          binding_source = "port_store_scoped_at_or_before";
        }
      }
      if (bound_value.empty() && request.port_store != nullptr) {
        const auto scoped_latest =
            request.port_store->read_node_port_output_scoped(
                request.branch_id,
                request.timeline_id,
                source_node_id,
                source_port_id);
        if (scoped_latest.has_value()) {
          bound_value = valueFromRuntimePortPacket(scoped_latest.value());
          binding_source = "port_store_scoped_latest";
        }
      }
      if (bound_value.empty()) {
        result.evidence["missing_explicit_bindings"].push_back(
            bindingEvidenceBase(node_id, target_port_id, source_node_id, source_port_id, binding_source));
        continue;
      }

      result.upstream[target_port_id] = bound_value;
      result.upstream["input_ports"][target_port_id] = bound_value;
      nlohmann::json binding_record = {
          {"binding_id", jsonString(binding, "binding_id")},
          {"source_node_id", source_node_id},
          {"source_port_id", source_port_id},
          {"target_node_id", node_id},
          {"target_port_id", target_port_id},
          {"binding_source", binding_source},
          {"branch_id", request.branch_id},
          {"timeline_id", request.timeline_id},
      };
      result.upstream["edge_bindings"].push_back(binding_record);
      result.evidence["bindings"].push_back(binding_record);
      result.evidence["explicit_binding_count"] =
          result.evidence.value("explicit_binding_count", 0) + 1;
    }
  }

  const nlohmann::json target_inputs =
      request.data_plane_info.value("inputs", nlohmann::json::array());
  if (request.allow_implicit_contract_port_binding && target_inputs.is_array() &&
      request.node.contains("depends_on") && request.node.at("depends_on").is_array()) {
    ensureBindingContainers(result.upstream);
    for (const auto& input_spec : target_inputs) {
      if (!input_spec.is_object()) {
        continue;
      }
      const std::string target_port_id = jsonString(input_spec, "port_id");
      const std::string target_contract_id = jsonString(input_spec, "contract_id");
      const std::string target_field = normalizedPortToken(target_port_id);
      if (target_port_id.empty()) {
        continue;
      }
      for (const auto& dep : request.node.at("depends_on")) {
        if (!dep.is_string()) {
          continue;
        }
        const std::string source_node_id = dep.get<std::string>();
        if (!request.current_outputs.is_object() || !request.current_outputs.contains(source_node_id)) {
          continue;
        }
        const nlohmann::json source_ports = outputPorts(request.current_outputs.at(source_node_id));
        if (!source_ports.is_object()) {
          continue;
        }
        bool injected = false;
        for (auto it = source_ports.begin(); it != source_ports.end(); ++it) {
          if (!it.value().is_object()) {
            continue;
          }
          const std::string source_contract_id = jsonString(it.value(), "contract_id");
          const std::string source_port_id = jsonString(it.value(), "port_id", it.key());
          const std::string source_field =
              jsonString(it.value(), "field_name", normalizedPortToken(source_port_id));
          const bool contract_match =
              !target_contract_id.empty() && source_contract_id == target_contract_id;
          const bool field_match = !target_field.empty() && source_field == target_field;
          if (!contract_match && !field_match) {
            continue;
          }
          result.upstream[target_port_id] = it.value();
          result.upstream["input_ports"][target_port_id] = it.value();
          nlohmann::json binding_record = {
              {"source_node_id", source_node_id},
              {"source_port_id", source_port_id},
              {"target_node_id", node_id},
              {"target_port_id", target_port_id},
              {"match", contract_match ? "contract_id" : "field_name"},
              {"binding_source", "implicit_contract_port_binding"},
          };
          result.upstream["edge_bindings"].push_back(binding_record);
          result.evidence["bindings"].push_back(binding_record);
          result.evidence["implicit_binding_count"] =
              result.evidence.value("implicit_binding_count", 0) + 1;
          trace(
              request,
              "input_port_injected target=" + node_id +
                  " port=" + target_port_id +
                  " source=" + source_node_id +
                  " source_port=" + source_port_id);
          injected = true;
          break;
        }
        if (injected) {
          break;
        }
      }
    }
  }

  result.evidence["upstream_has_input_ports"] =
      result.upstream.contains("input_ports") && result.upstream.at("input_ports").is_object();
  result.evidence["upstream_has_edge_bindings"] =
      result.upstream.contains("edge_bindings") && result.upstream.at("edge_bindings").is_array();
  result.evidence["implicit_binding_enabled"] = request.allow_implicit_contract_port_binding;
  return result;
}

}  // namespace FlightEnvPlatformRuntime
