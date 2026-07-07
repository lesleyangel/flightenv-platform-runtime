#pragma once

#include "FlightEnvPlatformRuntime/NativeWorkflowRunner.hpp"

#include <map>
#include <string>
#include <vector>

namespace FlightEnvPlatformRuntime {

struct NativeWorkflowCompiledState {
  nlohmann::json execution_plan = nlohmann::json::object();
  nlohmann::json time_plan = nlohmann::json::object();
  nlohmann::json scheduler_plan = nlohmann::json::object();
  nlohmann::json scheduler_table = nlohmann::json::object();
  nlohmann::json uncertainty_plan = nlohmann::json::object();
  nlohmann::json state_store_plan = nlohmann::json::object();
  nlohmann::json data_plane_plan = nlohmann::json::object();
  nlohmann::json estimation_plan = nlohmann::json::object();
  nlohmann::json edge_binding_plan = nlohmann::json::object();
  nlohmann::json rate_transition_plan = nlohmann::json::object();
  nlohmann::json workflow_snapshot = nlohmann::json::object();
  nlohmann::json operator_snapshot = nlohmann::json::object();
  nlohmann::json resource_lock = nlohmann::json::object();
  nlohmann::json model_snapshot = nlohmann::json::object();
  nlohmann::json adapter_registry = nlohmann::json::object();
  nlohmann::json edge_bindings_by_target = nlohmann::json::object();
  nlohmann::json rate_transitions_by_binding = nlohmann::json::object();
  nlohmann::json rate_transitions_by_target = nlohmann::json::object();
  std::vector<nlohmann::json> nodes;
  std::map<std::string, nlohmann::json> operator_by_id;
  std::map<std::string, nlohmann::json> resource_by_id;
  std::map<std::string, nlohmann::json> model_binding_by_operator;
  std::string workflow_id = "workflow";
  std::string object_id = "object";

  nlohmann::json resolveAdapterEntry(
      const nlohmann::json& node,
      bool allow_wildcard_adapter) const;
};

NativeWorkflowCompiledState loadNativeWorkflowCompiledState(
    const NativeWorkflowOptions& options);

}  // namespace FlightEnvPlatformRuntime
