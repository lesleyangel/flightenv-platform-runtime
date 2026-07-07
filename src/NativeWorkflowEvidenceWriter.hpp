#pragma once

#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace flightenv::platform {
class ReadyQueueScheduler;
class ThreadPoolExecutorDescriptor;
class ThreadSafePortStore;
}  // namespace flightenv::platform

namespace FlightEnvPlatformRuntime {

class RuntimeReadyQueueExecutor;

struct NativeWorkflowEvidenceWriteRequest {
  std::filesystem::path run_dir;
  std::filesystem::path adapter_registry_ref;
  std::filesystem::path compiled_workflow_dir;
  std::filesystem::path external_observation_stream_ref;
  std::string run_id;
  std::string workflow_id;
  std::string object_id;
  std::string branch_id;
  std::string timeline_id;
  std::string zero_copy_mode;
  std::string typed_buffer_persistence;
  std::size_t node_count = 0;
  std::size_t adapter_session_count = 0;

  const nlohmann::json* lifecycle_events = nullptr;
  const nlohmann::json* scheduler_events = nullptr;
  const nlohmann::json* node_snapshots = nullptr;
  const nlohmann::json* uncertainty_nodes = nullptr;
  const nlohmann::json* checkpoints = nullptr;
  const nlohmann::json* data_plane_entries = nullptr;
  const nlohmann::json* input_artifacts = nullptr;
  const nlohmann::json* output_artifacts = nullptr;
  const nlohmann::json* loop_iterations = nullptr;
  const nlohmann::json* runtime_packets = nullptr;
  const nlohmann::json* runtime_rate_transition_nodes = nullptr;
  const nlohmann::json* outputs = nullptr;
  const nlohmann::json* forecast_uncertainty_events = nullptr;
  const nlohmann::json* forecast_uncertainty_summary = nullptr;
  const nlohmann::json* seed = nullptr;
  const nlohmann::json* external_observations = nullptr;
  const nlohmann::json* session_summary = nullptr;
  const nlohmann::json* backend_capability_report = nullptr;
  const nlohmann::json* edge_binding_plan = nullptr;
  const nlohmann::json* rate_transition_plan = nullptr;
  const nlohmann::json* scheduler_table = nullptr;
  const nlohmann::json* time_plan = nullptr;
  const nlohmann::json* scheduler_plan = nullptr;

  const flightenv::platform::ThreadSafePortStore* port_store = nullptr;
  const flightenv::platform::ReadyQueueScheduler* pdk_scheduler = nullptr;
  const flightenv::platform::ThreadPoolExecutorDescriptor* pdk_executor = nullptr;
  const RuntimeReadyQueueExecutor* ready_executor = nullptr;

  int iteration_count = 0;
  int failed_nodes = 0;
  bool prepare_only = false;
};

void writeNativeWorkflowEvidence(const NativeWorkflowEvidenceWriteRequest& request);

}  // namespace FlightEnvPlatformRuntime
