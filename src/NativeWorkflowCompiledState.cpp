#include "NativeWorkflowCompiledState.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace FlightEnvPlatformRuntime {
namespace {

std::string pathString(const fs::path& path) {
  return path.lexically_normal().string();
}

json readJson(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("JSON file not found: " + pathString(path));
  }
  json value;
  input >> value;
  return value;
}

json readJsonIfExists(const fs::path& path) {
  if (!fs::exists(path)) {
    return json::object();
  }
  return readJson(path);
}

std::string jsonString(const json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

bool envFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return false;
  }
  std::string text(value);
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text == "1" || text == "true" || text == "on" || text == "yes";
}

json requireArray(const json& value, const std::string& key, const fs::path& source) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_array()) {
    throw std::runtime_error("JSON array '" + key + "' missing in " + pathString(source));
  }
  return value.at(key);
}

std::vector<json> topoSortNodes(const json& nodes_array) {
  std::vector<json> nodes;
  std::set<std::string> known_ids;
  for (const auto& item : nodes_array) {
    if (item.is_object()) {
      const std::string node_id = jsonString(item, "node_id");
      if (node_id.empty()) {
        throw std::runtime_error("Workflow execution_plan contains a node with empty node_id.");
      }
      if (!known_ids.insert(node_id).second) {
        throw std::runtime_error("Workflow execution_plan contains duplicate node_id: " + node_id);
      }
      nodes.push_back(item);
    }
  }
  for (const auto& node : nodes) {
    const std::string node_id = jsonString(node, "node_id");
    if (!node.contains("depends_on") || !node.at("depends_on").is_array()) {
      continue;
    }
    for (const auto& dep : node.at("depends_on")) {
      if (!dep.is_string()) {
        throw std::runtime_error("Workflow dependency for node " + node_id + " is not a string.");
      }
      const std::string dep_id = dep.get<std::string>();
      if (!known_ids.count(dep_id)) {
        throw std::runtime_error("Workflow node " + node_id + " depends on missing node: " + dep_id);
      }
    }
  }
  std::vector<json> sorted;
  std::set<std::string> done;
  while (sorted.size() < nodes.size()) {
    bool progressed = false;
    for (const auto& node : nodes) {
      const std::string node_id = jsonString(node, "node_id");
      if (node_id.empty() || done.count(node_id)) {
        continue;
      }
      bool ready = true;
      if (node.contains("depends_on") && node.at("depends_on").is_array()) {
        for (const auto& dep : node.at("depends_on")) {
          if (dep.is_string() && !done.count(dep.get<std::string>())) {
            ready = false;
            break;
          }
        }
      }
      if (ready) {
        sorted.push_back(node);
        done.insert(node_id);
        progressed = true;
      }
    }
    if (!progressed) {
      std::ostringstream details;
      bool first_node = true;
      for (const auto& node : nodes) {
        const std::string node_id = jsonString(node, "node_id");
        if (node_id.empty() || done.count(node_id)) {
          continue;
        }
        if (!first_node) {
          details << "; ";
        }
        first_node = false;
        details << node_id << " waits_for=[";
        bool first_dep = true;
        if (node.contains("depends_on") && node.at("depends_on").is_array()) {
          for (const auto& dep : node.at("depends_on")) {
            const std::string dep_id = dep.is_string() ? dep.get<std::string>() : "<non-string>";
            if (done.count(dep_id)) {
              continue;
            }
            if (!first_dep) {
              details << ",";
            }
            first_dep = false;
            details << dep_id;
          }
        }
        details << "]";
      }
      throw std::runtime_error(
          "Workflow dependency graph is cyclic or unresolved; fail-fast instead of executing an invalid order. " +
          details.str());
    }
  }
  return sorted;
}

void loadAdapterRegistry(
    NativeWorkflowCompiledState& state,
    const NativeWorkflowOptions& options) {
  if (options.adapter_registry.empty() || !fs::exists(options.adapter_registry)) {
    if (options.require_adapter_registry) {
      throw std::runtime_error("Adapter registry not found: " + pathString(options.adapter_registry));
    }
    state.adapter_registry = json::object();
    return;
  }
  state.adapter_registry = readJson(options.adapter_registry);
}

}  // namespace

json NativeWorkflowCompiledState::resolveAdapterEntry(
    const json& node,
    bool allow_wildcard_adapter) const {
  if (!adapter_registry.is_object() || !adapter_registry.contains("adapters") ||
      !adapter_registry.at("adapters").is_array()) {
    return json::object();
  }
  const std::string adapter_id = jsonString(node, "adapter_id");
  const std::string execution_kind = jsonString(node, "execution_kind");
  json wildcard = json::object();
  json by_kind = json::object();
  for (const auto& entry : adapter_registry.at("adapters")) {
    if (!entry.is_object()) {
      continue;
    }
    const std::string id = jsonString(entry, "adapter_id");
    if (id == adapter_id) {
      return entry;
    }
    if (id == execution_kind && by_kind.empty()) {
      by_kind = entry;
    }
    if (id == "*" && allow_wildcard_adapter && wildcard.empty()) {
      wildcard = entry;
    }
  }
  if (!by_kind.empty()) {
    return by_kind;
  }
  return wildcard;
}

NativeWorkflowCompiledState loadNativeWorkflowCompiledState(
    const NativeWorkflowOptions& options) {
  if (!fs::exists(options.compiled_workflow)) {
    throw std::runtime_error("Compiled workflow not found: " + pathString(options.compiled_workflow));
  }

  NativeWorkflowCompiledState state;
  state.execution_plan = readJson(options.compiled_workflow / "execution_plan.json");
  state.time_plan = readJsonIfExists(options.compiled_workflow / "time_plan.json");
  state.scheduler_plan = readJsonIfExists(options.compiled_workflow / "scheduler_plan.json");
  state.scheduler_table = readJsonIfExists(options.compiled_workflow / "scheduler_table.json");
  state.uncertainty_plan = readJsonIfExists(options.compiled_workflow / "uncertainty_plan.json");
  state.state_store_plan = readJsonIfExists(options.compiled_workflow / "state_store_plan.json");
  state.data_plane_plan = readJsonIfExists(options.compiled_workflow / "data_plane_plan.json");
  state.estimation_plan = readJsonIfExists(options.compiled_workflow / "estimation_plan.json");
  state.edge_binding_plan = readJson(options.compiled_workflow / "edge_binding_plan.json");

  const fs::path rate_transition_plan_path = options.compiled_workflow / "rate_transition_plan.json";
  const bool has_rate_transition_plan = fs::exists(rate_transition_plan_path);
  state.rate_transition_plan =
      has_rate_transition_plan
          ? readJson(rate_transition_plan_path)
          : json{
                {"schema_version", "flightenv.platform.rate_transition_plan.v1"},
                {"transitions", json::array()},
                {"summary",
                 {{"transition_count", 0},
                  {"same_rate_transition_count", 0},
                  {"cross_rate_transition_count", 0},
                  {"fast_to_slow_count", 0},
                  {"slow_to_fast_count", 0},
                  {"runtime_transition_count", 0}}},
            };

  state.workflow_snapshot = readJsonIfExists(options.compiled_workflow / "workflow_snapshot.json");
  state.operator_snapshot = readJsonIfExists(options.compiled_workflow / "operator_snapshot.json");
  state.resource_lock = readJsonIfExists(options.compiled_workflow / "resource_lock.json");
  state.model_snapshot = readJsonIfExists(options.compiled_workflow / "model_snapshot.json");

  json execution_nodes = requireArray(
      state.execution_plan,
      "nodes",
      options.compiled_workflow / "execution_plan.json");

  const json compiled_transitions = state.rate_transition_plan.value("transitions", json::array());
  if (compiled_transitions.is_array()) {
    for (const auto& transition : compiled_transitions) {
      if (!transition.is_object()) {
        continue;
      }
      const std::string transition_id = jsonString(transition, "transition_id");
      const std::string binding_id = jsonString(transition, "binding_id");
      const std::string target_node_id = jsonString(transition, "target_node_id");
      const std::string source_node_id = jsonString(transition, "source_node_id");
      const std::string source_port_id = jsonString(transition, "source_port_id");
      const std::string target_port_id = jsonString(transition, "target_port_id");
      if (transition_id.empty() || binding_id.empty() || target_node_id.empty() ||
          source_node_id.empty() || source_port_id.empty() || target_port_id.empty()) {
        throw std::runtime_error("rate_transition_plan.json contains an incomplete transition");
      }
      if (state.rate_transitions_by_binding.contains(binding_id)) {
        throw std::runtime_error(
            "rate_transition_plan.json contains duplicate transition for binding: " + binding_id);
      }
      state.rate_transitions_by_binding[binding_id] = transition;
      state.rate_transitions_by_target[target_node_id].push_back(transition);
    }
  }

  const json compiled_bindings = state.edge_binding_plan.value("bindings", json::array());
  if (compiled_bindings.is_array()) {
    for (const auto& binding : compiled_bindings) {
      if (!binding.is_object()) {
        continue;
      }
      const std::string target_node_id = jsonString(binding, "target_node_id");
      const std::string source_node_id = jsonString(binding, "source_node_id");
      const std::string source_port_id = jsonString(binding, "source_port_id");
      const std::string target_port_id = jsonString(binding, "target_port_id");
      if (target_node_id.empty() || source_node_id.empty() ||
          source_port_id.empty() || target_port_id.empty()) {
        throw std::runtime_error("edge_binding_plan.json contains an incomplete binding");
      }
      json enriched_binding = binding;
      const std::string binding_id = jsonString(enriched_binding, "binding_id");
      if (!binding_id.empty() && state.rate_transitions_by_binding.contains(binding_id)) {
        enriched_binding["rate_transition"] = state.rate_transitions_by_binding.at(binding_id);
      } else if (has_rate_transition_plan) {
        throw std::runtime_error(
            "rate_transition_plan.json is missing a transition for edge binding: " + binding_id);
      }
      state.edge_bindings_by_target[target_node_id].push_back(enriched_binding);
    }
  }

  if (state.edge_bindings_by_target.empty() &&
      envFlagEnabled("FLIGHTENV_ALLOW_WORKFLOW_SNAPSHOT_EDGE_BINDING_FALLBACK")) {
    const json workflow = state.workflow_snapshot.value("workflow", json::object());
    const json phases = workflow.value("phases", json::array());
    if (phases.is_array()) {
      for (const auto& phase : phases) {
        if (!phase.is_object()) {
          continue;
        }
        const std::string phase_id = jsonString(phase, "phase_id");
        const json stages = phase.value("stages", json::array());
        if (!stages.is_array()) {
          continue;
        }
        for (const auto& stage : stages) {
          if (!stage.is_object()) {
            continue;
          }
          const std::string stage_id = jsonString(stage, "stage_id");
          const json subgraph = stage.value("subgraph", json::object());
          const json edges = subgraph.value("edges", json::array());
          if (!edges.is_array()) {
            continue;
          }
          for (const auto& edge : edges) {
            if (!edge.is_object()) {
              continue;
            }
            const json from = edge.value("from", json::object());
            const json to = edge.value("to", json::object());
            const std::string from_node = jsonString(from, "node_id");
            const std::string to_node = jsonString(to, "node_id");
            if (from_node.empty() || to_node.empty()) {
              continue;
            }
            const std::string source_node_id = phase_id + "." + stage_id + "." + from_node;
            const std::string target_node_id = phase_id + "." + stage_id + "." + to_node;
            state.edge_bindings_by_target[target_node_id].push_back({
                {"source_node_id", source_node_id},
                {"source_port_id", jsonString(from, "port_id")},
                {"target_node_id", target_node_id},
                {"target_port_id", jsonString(to, "port_id")},
                {"source", "workflow_snapshot_fallback"},
            });
          }
        }
      }
    }
  }

  for (auto& node : execution_nodes) {
    if (!node.is_object()) {
      continue;
    }
    const std::string node_id = jsonString(node, "node_id");
    if (!state.edge_bindings_by_target.contains(node_id)) {
      continue;
    }
    if (!node.contains("depends_on") || !node.at("depends_on").is_array()) {
      node["depends_on"] = json::array();
    }
    std::set<std::string> deps;
    for (const auto& dep : node.at("depends_on")) {
      if (dep.is_string()) {
        deps.insert(dep.get<std::string>());
      }
    }
    for (const auto& binding : state.edge_bindings_by_target.at(node_id)) {
      const std::string source_node_id = jsonString(binding, "source_node_id");
      if (!source_node_id.empty() && deps.insert(source_node_id).second) {
        node["depends_on"].push_back(source_node_id);
      }
    }
    node["edge_bindings"] = state.edge_bindings_by_target.at(node_id);
    if (state.rate_transitions_by_target.contains(node_id)) {
      node["rate_transitions"] = state.rate_transitions_by_target.at(node_id);
    }
  }

  state.nodes = topoSortNodes(execution_nodes);
  state.workflow_id = jsonString(state.execution_plan, "workflow_id", "workflow");
  state.object_id = jsonString(state.execution_plan, "object_id", "object");

  for (const auto& item : state.operator_snapshot.value("operators", json::array())) {
    state.operator_by_id[jsonString(item, "operator_id")] = item;
  }
  for (const auto& item : state.resource_lock.value("resources", json::array())) {
    state.resource_by_id[jsonString(item, "resource_id")] = item;
  }
  for (const auto& item : state.model_snapshot.value("operator_bindings", json::array())) {
    state.model_binding_by_operator[jsonString(item, "operator_id")] = item;
  }

  loadAdapterRegistry(state, options);
  return state;
}

}  // namespace FlightEnvPlatformRuntime
