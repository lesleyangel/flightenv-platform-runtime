#include "NativeWorkflowEvidenceWriter.hpp"

#include "NativeWorkflowNodeResultCommit.hpp"

#include "FlightEnvPlatform/Runtime/ReadyQueueScheduler.hpp"
#include "FlightEnvPlatform/Runtime/RuntimePacket.hpp"
#include "FlightEnvPlatform/Runtime/ThreadPoolExecutor.hpp"
#include "FlightEnvPlatform/Runtime/ThreadSafePortStore.hpp"
#include "FlightEnvPlatformRuntime/AdapterSession.hpp"
#include "FlightEnvPlatformRuntime/RuntimeDueTimeScheduler.hpp"
#include "FlightEnvPlatformRuntime/RuntimeEvidenceWriter.hpp"
#include "FlightEnvPlatformRuntime/RuntimeReadyQueueExecutor.hpp"
#include "FlightEnvPlatformRuntime/RuntimeScheduleDiagnostics.hpp"
#include "FlightEnvPlatformRuntime/RuntimeTypedBufferStore.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <set>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
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
  gmtime_r(&tm, &time);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

std::string pathString(const fs::path& path) {
  return path.lexically_normal().string();
}

std::string jsonString(const json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

double jsonDouble(const json& value, const std::string& key, double fallback = 0.0) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
    return fallback;
  }
  return value.at(key).get<double>();
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

bool jsonBool(const json& value, const std::string& key, bool fallback = false) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean()) {
    return fallback;
  }
  return value.at(key).get<bool>();
}

struct SchedulerRuntimeEventCounts {
  int input_alignment_blocked_count = 0;
  int ready_queue_rejected_count = 0;
  int node_due_retry_scheduled_count = 0;
  int node_due_dropped_count = 0;
};

SchedulerRuntimeEventCounts countSchedulerRuntimeEvents(const json& scheduler_events) {
  SchedulerRuntimeEventCounts counts;
  if (!scheduler_events.is_array()) {
    return counts;
  }
  for (const auto& event : scheduler_events) {
    if (!event.is_object()) {
      continue;
    }
    const std::string event_name = jsonString(event, "event");
    if (event_name == "input_alignment_blocked") {
      ++counts.input_alignment_blocked_count;
    } else if (event_name == "ready_queue_admission" && !jsonBool(event, "accepted", true)) {
      ++counts.ready_queue_rejected_count;
    } else if (event_name == "node_due_retry_scheduled") {
      ++counts.node_due_retry_scheduled_count;
    } else if (event_name == "node_due_dropped") {
      ++counts.node_due_dropped_count;
    }
  }
  return counts;
}

json readyQueueSchedulerEvidence(const flightenv::platform::ReadyQueueScheduler& scheduler) {
  return {
      {"type", "FlightEnvPlatform::ReadyQueueScheduler"},
      {"policy",
       {
           {"max_parallelism", scheduler.policy.max_parallelism},
           {"ready_queue_policy", scheduler.policy.ready_queue_policy},
           {"resource_conflict_policy", scheduler.policy.resource_conflict_policy},
           {"deadline_policy", scheduler.policy.deadline_policy},
           {"default_deadline_s", scheduler.policy.default_deadline_s},
           {"record_timeline", scheduler.policy.record_timeline},
       }},
      {"plan_node_count", scheduler.plan_nodes.size()},
      {"effective_max_parallelism", scheduler.max_parallelism()},
  };
}

json threadPoolExecutorEvidence(const flightenv::platform::ThreadPoolExecutorDescriptor& executor) {
  return {
      {"type", "FlightEnvPlatform::ThreadPoolExecutorDescriptor"},
      {"options",
       {
           {"max_workers", executor.options.max_workers},
           {"worker_name_prefix", executor.options.worker_name_prefix},
       }},
  };
}

json runtimePacketArrayFromPortStore(const flightenv::platform::ThreadSafePortStore& port_store) {
  json packets = json::array();
  for (const auto& packet : port_store.snapshot_packets()) {
    packets.push_back(nativeWorkflowRuntimePacketToJson(packet));
  }
  return packets;
}

json portStoreEvidence(const flightenv::platform::ThreadSafePortStore& port_store) {
  const std::vector<flightenv::platform::RuntimePacket> packets = port_store.snapshot_packets();
  std::set<std::string> output_nodes;
  std::set<std::string> ports;
  std::set<std::string> logical_node_ports;
  std::set<std::string> scoped_logical_node_ports;
  for (const auto& packet : packets) {
    if (!packet.node_id.empty()) {
      output_nodes.insert(packet.node_id);
    }
    if (!packet.port_name.empty()) {
      ports.insert(packet.port_name);
    }
    const auto port_id = packet.tags.find("port_id");
    if (!packet.node_id.empty() && port_id != packet.tags.end() && !port_id->second.empty()) {
      logical_node_ports.insert(packet.node_id + "." + port_id->second);
      const auto branch_id = packet.tags.find("branch_id");
      const auto timeline_id = packet.tags.find("timeline_id");
      scoped_logical_node_ports.insert(
          (branch_id == packet.tags.end() ? std::string() : branch_id->second) + "|" +
          (timeline_id == packet.tags.end() ? std::string() : timeline_id->second) + "|" +
          packet.node_id + "." + port_id->second);
    }
  }
  return {
      {"type", "FlightEnvPlatform::ThreadSafePortStore"},
      {"packet_count", packets.size()},
      {"port_count", ports.size()},
      {"node_output_count", output_nodes.size()},
      {"port_packet_count", logical_node_ports.size()},
      {"scoped_port_packet_count", scoped_logical_node_ports.size()},
  };
}

const json& requiredJson(const json* value, const char* name) {
  if (value == nullptr) {
    throw std::runtime_error(std::string("Native workflow evidence request missing JSON field: ") + name);
  }
  return *value;
}

template <typename T>
const T& requiredPtr(const T* value, const char* name) {
  if (value == nullptr) {
    throw std::runtime_error(std::string("Native workflow evidence request missing runtime field: ") + name);
  }
  return *value;
}

json buildSensorStreamArtifact(const NativeWorkflowEvidenceWriteRequest& request, const json& observations) {
  json frames = json::array();
  if (observations.is_array()) {
    for (std::size_t i = 0; i < observations.size(); ++i) {
      const json& frame = observations.at(i);
      frames.push_back({
          {"frame_index", static_cast<int>(i)},
          {"loop_iteration_index", static_cast<int>(i)},
          {"sample_time_s", jsonDouble(frame, "sample_time_s", jsonDouble(frame, "time_s", static_cast<double>(i)))},
          {"sensor_count", frame.value("sensor_count", 0)},
          {"source", "external_observation_stream"},
          {"selected_state", frame.value("selected_state", frame.value("state", json::object()))},
          {"frame", frame},
      });
    }
  }
  return {
      {"schema_version", "flightenv.platform.sensor_stream.v1"},
      {"run_id", request.run_id},
      {"workflow_id", request.workflow_id},
      {"object_id", request.object_id},
      {"generated_at_utc", nowUtcIso()},
      {"source_operator_id", "external.measurement_driver"},
      {"source_stream_path", pathString(request.external_observation_stream_ref)},
      {"frames", frames},
      {"summary",
       {
           {"frame_count", frames.size()},
           {"input_exhausted", false},
       }},
  };
}

}  // namespace

void writeNativeWorkflowEvidence(const NativeWorkflowEvidenceWriteRequest& request) {
  const json& lifecycle_events = requiredJson(request.lifecycle_events, "lifecycle_events");
  const json& scheduler_events = requiredJson(request.scheduler_events, "scheduler_events");
  const json& node_snapshots = requiredJson(request.node_snapshots, "node_snapshots");
  const json& uncertainty_nodes = requiredJson(request.uncertainty_nodes, "uncertainty_nodes");
  const json& checkpoints = requiredJson(request.checkpoints, "checkpoints");
  const json& data_plane_entries = requiredJson(request.data_plane_entries, "data_plane_entries");
  const json& input_artifacts = requiredJson(request.input_artifacts, "input_artifacts");
  const json& output_artifacts = requiredJson(request.output_artifacts, "output_artifacts");
  const json& loop_iterations = requiredJson(request.loop_iterations, "loop_iterations");
  const json& runtime_packets = requiredJson(request.runtime_packets, "runtime_packets");
  const json& runtime_rate_transition_nodes =
      requiredJson(request.runtime_rate_transition_nodes, "runtime_rate_transition_nodes");
  const json& outputs = requiredJson(request.outputs, "outputs");
  const json& forecast_uncertainty_events =
      requiredJson(request.forecast_uncertainty_events, "forecast_uncertainty_events");
  const json& forecast_uncertainty_summary =
      requiredJson(request.forecast_uncertainty_summary, "forecast_uncertainty_summary");
  const json& seed = requiredJson(request.seed, "seed");
  const json& external_observations = requiredJson(request.external_observations, "external_observations");
  const json& session_summary = requiredJson(request.session_summary, "session_summary");
  const json& backend_capability_report =
      requiredJson(request.backend_capability_report, "backend_capability_report");
  const json& edge_binding_plan = requiredJson(request.edge_binding_plan, "edge_binding_plan");
  const json& rate_transition_plan = requiredJson(request.rate_transition_plan, "rate_transition_plan");
  const json& scheduler_table = requiredJson(request.scheduler_table, "scheduler_table");
  const json& time_plan = requiredJson(request.time_plan, "time_plan");
  const json& scheduler_plan = requiredJson(request.scheduler_plan, "scheduler_plan");
  const auto& port_store = requiredPtr(request.port_store, "port_store");
  const auto& pdk_scheduler = requiredPtr(request.pdk_scheduler, "pdk_scheduler");
  const auto& pdk_executor = requiredPtr(request.pdk_executor, "pdk_executor");
  const auto& ready_executor = requiredPtr(request.ready_executor, "ready_executor");

  const std::string status =
      request.failed_nodes == 0 ? (request.prepare_only ? "prepared" : "completed") : "failed";
  const RuntimeEvidenceWriter evidence_writer(request.run_dir);
  int held_output_total = 0;
  double summary_base_dt_s = 0.0;
  double summary_output_period_s = 0.0;
  if (loop_iterations.is_array()) {
    for (const auto& item : loop_iterations) {
      if (!item.is_object()) {
        continue;
      }
      held_output_total += jsonInt(item, "held_output_count", 0);
      if (summary_base_dt_s <= 0.0) {
        summary_base_dt_s = jsonDouble(item, "base_dt_s", 0.0);
      }
      if (summary_output_period_s <= 0.0) {
        summary_output_period_s = jsonDouble(item, "output_period_s", 0.0);
      }
    }
  }
  const json port_store_packets = runtimePacketArrayFromPortStore(port_store);
  const json port_store_summary = portStoreEvidence(port_store);
  const json typed_buffer_store_summary = RuntimeTypedBufferStore::instance().summary();
  const SchedulerRuntimeEventCounts scheduler_runtime_event_counts =
      countSchedulerRuntimeEvents(scheduler_events);
  const json pdk_runtime_core = {
      {"runtime_packet", "FlightEnvPlatform::RuntimePacket"},
      {"port_store", port_store_summary},
      {"ready_queue_scheduler", readyQueueSchedulerEvidence(pdk_scheduler)},
      {"thread_pool_executor", threadPoolExecutorEvidence(pdk_executor)},
      {"ready_queue_executor", ready_executor.runtimeCoreEvidence()},
      {"adapter_session_contract", kAdapterSessionContract},
      {"legacy_json_packet_buffer_count", runtime_packets.size()},
      {"typed_packet_fields",
       {"contract_id",
        "typed_schema_id",
        "typed_dto_name",
        "typed_payload_ref",
        "typed_buffer_ref",
        "buffer_layout_id",
        "buffer_bytes",
        "zero_copy_eligible"}},
      {"zero_copy_mode", request.zero_copy_mode},
      {"typed_buffer_persistence", request.typed_buffer_persistence},
      {"single_kernel_stage", "R2.runtime_packet_typed_ref_bridge"},
      {"remaining_host_owned_services",
       {"online_loop_clock", "branch_manager", "runtime_index_writer", "health_ledger_writer"}},
  };
  evidence_writer.writeJson(
      "adapter_lifecycle_log.json",
      {{"schema_version", "flightenv.platform.adapter_lifecycle_log.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"generated_at_utc", nowUtcIso()},
       {"execution_backend", "native_adapter_sessions"},
       {"events", lifecycle_events}});
  evidence_writer.writeJson(
      "scheduler_timeline.json",
      {{"schema_version", "flightenv.platform.scheduler_timeline.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"generated_at_utc", nowUtcIso()},
       {"events", scheduler_events}});
  evidence_writer.writeJson(
      "runtime_node_snapshot.json",
      {{"schema_version", "flightenv.platform.runtime_node_snapshot.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"generated_at_utc", nowUtcIso()},
       {"nodes", node_snapshots}});
  evidence_writer.writeJson(
      "runtime_outputs.json",
      {{"schema_version", "flightenv.platform.runtime_outputs.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"generated_at_utc", nowUtcIso()},
       {"last_loop_iteration_index", std::max(0, request.iteration_count - 1)},
       {"typed_buffer_store", typed_buffer_store_summary},
       {"outputs", outputs}});
  evidence_writer.writeJson(
      "runtime_loop_summary.json",
      {{"schema_version", "flightenv.platform.runtime_loop_summary.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"generated_at_utc", nowUtcIso()},
       {"iterations", loop_iterations},
       {"summary",
        {{"iteration_count", request.iteration_count},
         {"failed_nodes", request.failed_nodes},
         {"base_dt_s", summary_base_dt_s},
         {"output_period_s", summary_output_period_s},
         {"held_output_count", held_output_total},
         {"input_alignment_blocked_count",
          scheduler_runtime_event_counts.input_alignment_blocked_count},
         {"ready_queue_rejected_count",
          scheduler_runtime_event_counts.ready_queue_rejected_count},
         {"node_due_retry_scheduled_count",
          scheduler_runtime_event_counts.node_due_retry_scheduled_count},
         {"node_due_dropped_count",
          scheduler_runtime_event_counts.node_due_dropped_count}}}});
  evidence_writer.writeJson(
      "runtime_packets.json",
      {{"schema_version", "flightenv.platform.runtime_packets.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"branch_id", request.branch_id},
       {"timeline_id", request.timeline_id},
       {"generated_at_utc", nowUtcIso()},
       {"producer", "ThreadSafePortStore"},
       {"summary", port_store_summary},
       {"packets", port_store_packets}});
  int runtime_rate_transition_executed_count = 0;
  int runtime_rate_transition_blocked_count = 0;
  if (runtime_rate_transition_nodes.is_array()) {
    for (const auto& item : runtime_rate_transition_nodes) {
      if (!item.is_object()) {
        continue;
      }
      if (jsonString(item, "status") == "executed") {
        ++runtime_rate_transition_executed_count;
      } else if (jsonString(item, "status") == "blocked") {
        ++runtime_rate_transition_blocked_count;
      }
    }
  }
  evidence_writer.writeJson(
      "runtime_rate_transition_nodes.json",
      {{"schema_version", "flightenv.platform.runtime_rate_transition_nodes.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"branch_id", request.branch_id},
       {"timeline_id", request.timeline_id},
       {"generated_at_utc", nowUtcIso()},
       {"nodes", runtime_rate_transition_nodes},
       {"summary",
        {{"event_count", runtime_rate_transition_nodes.size()},
         {"executed_count", runtime_rate_transition_executed_count},
         {"blocked_count", runtime_rate_transition_blocked_count}}}});
  evidence_writer.writeJson(
      "data_plane_manifest.json",
      {{"schema_version", "flightenv.platform.data_plane_manifest.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"generated_at_utc", nowUtcIso()},
       {"entries", data_plane_entries},
       {"summary", {{"entry_count", data_plane_entries.size()}}}});
  evidence_writer.writeJson(
      "uncertainty_evidence.json",
      {{"schema_version", "flightenv.platform.uncertainty_evidence.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"generated_at_utc", nowUtcIso()},
       {"nodes", uncertainty_nodes}});
  evidence_writer.writeJson(
      "forecast_uncertainty_evidence.json",
      {{"schema_version", "flightenv.platform.forecast_uncertainty_evidence.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"branch_id", request.branch_id},
       {"timeline_id", request.timeline_id},
       {"generated_at_utc", nowUtcIso()},
       {"summary", forecast_uncertainty_summary},
       {"events", forecast_uncertainty_events}});
  evidence_writer.writeJson(
      "state_checkpoint.json",
      {{"schema_version", "flightenv.platform.state_checkpoint.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"generated_at_utc", nowUtcIso()},
       {"checkpoints", checkpoints}});
  evidence_writer.writeJson(
      "runtime_artifacts.json",
      {{"schema_version", "flightenv.platform.runtime_artifacts.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"generated_at_utc", nowUtcIso()},
       {"inputs", input_artifacts},
       {"outputs", output_artifacts}});
  evidence_writer.writeJson("sensor_stream.json", buildSensorStreamArtifact(request, external_observations));
  evidence_writer.writeJson("adapter_session_summary.json", session_summary);
  evidence_writer.writeJson("adapter_backend_capability_report.json", backend_capability_report);
  evidence_writer.writeJson("edge_binding_plan.json", edge_binding_plan);
  evidence_writer.writeJson("rate_transition_plan.json", rate_transition_plan);
  json scheduler_due_time_trace = json::object();
  json scheduler_diagnostics = json::object();
  if (!scheduler_table.empty()) {
    evidence_writer.writeJson("scheduler_table.json", scheduler_table);
    scheduler_due_time_trace = RuntimeDueTimeScheduler(scheduler_table).buildTrace();
    evidence_writer.writeJson("scheduler_due_time_trace.json", scheduler_due_time_trace);
    scheduler_diagnostics = RuntimeScheduleDiagnostics(
        scheduler_table,
        scheduler_due_time_trace,
        pdk_executor.options.max_workers)
                                .build();
    evidence_writer.writeJson("scheduler_diagnostics.json", scheduler_diagnostics);
  }
  const json time_plan_summary =
      time_plan.contains("summary") && time_plan.at("summary").is_object()
          ? time_plan.at("summary")
          : json::object();
  const json scheduler_plan_summary =
      scheduler_plan.contains("summary") && scheduler_plan.at("summary").is_object()
          ? scheduler_plan.at("summary")
          : json::object();
  const json scheduler_table_summary =
      scheduler_table.contains("summary") && scheduler_table.at("summary").is_object()
          ? scheduler_table.at("summary")
          : json::object();
  const json scheduler_due_time_trace_summary =
      scheduler_due_time_trace.contains("summary") && scheduler_due_time_trace.at("summary").is_object()
          ? scheduler_due_time_trace.at("summary")
          : json::object();
  const json scheduler_diagnostics_summary =
      scheduler_diagnostics.contains("summary") && scheduler_diagnostics.at("summary").is_object()
          ? scheduler_diagnostics.at("summary")
          : json::object();
  evidence_writer.writeJson(
      "runtime_evidence.json",
      {{"schema_version", "flightenv.platform.runtime_evidence.v1"},
       {"run_id", request.run_id},
       {"workflow_id", request.workflow_id},
       {"object_id", request.object_id},
       {"branch_id", request.branch_id},
       {"timeline_id", request.timeline_id},
       {"status", status},
       {"generated_at_utc", nowUtcIso()},
       {"execution_backend", "native_adapter_sessions"},
       {"runtime_core", pdk_runtime_core},
       {"adapter_registry_ref", pathString(request.adapter_registry_ref)},
       {"compiled_workflow_dir", pathString(request.compiled_workflow_dir)},
       {"initial_seed", seed},
       {"summary",
        {{"node_count", request.node_count},
         {"iteration_count", request.iteration_count},
         {"failed_nodes", request.failed_nodes},
         {"input_alignment_blocked_count",
          scheduler_runtime_event_counts.input_alignment_blocked_count},
         {"ready_queue_rejected_count",
          scheduler_runtime_event_counts.ready_queue_rejected_count},
         {"node_due_retry_scheduled_count",
          scheduler_runtime_event_counts.node_due_retry_scheduled_count},
         {"node_due_dropped_count",
          scheduler_runtime_event_counts.node_due_dropped_count},
         {"zero_copy_mode", request.zero_copy_mode},
         {"typed_buffer_persistence", request.typed_buffer_persistence},
         {"adapter_session_count", request.adapter_session_count},
         {"adapter_backend_capability", backend_capability_report.value("summary", json::object())},
         {"typed_buffer_store", typed_buffer_store_summary},
         {"runtime_packet_count", port_store_packets.size()},
         {"port_store_node_output_count", port_store_summary.value("node_output_count", 0)},
         {"runtime_rate_transition_node_event_count", runtime_rate_transition_nodes.size()},
         {"runtime_rate_transition_node_executed_count", runtime_rate_transition_executed_count},
         {"runtime_rate_transition_node_blocked_count", runtime_rate_transition_blocked_count},
         {"forecast_uncertainty_enabled",
          forecast_uncertainty_summary.value("enabled", false)},
         {"forecast_uncertainty_step_count",
          forecast_uncertainty_summary.value("step_count", 0)},
         {"forecast_uncertainty_seed_covariance_trace",
          forecast_uncertainty_summary.value("seed_covariance_trace", 0.0)},
         {"forecast_uncertainty_latest_covariance_trace",
          forecast_uncertainty_summary.value("latest_covariance_trace", 0.0)},
         {"forecast_uncertainty_growth_ratio",
          forecast_uncertainty_summary.value("growth_ratio", 0.0)},
         {"edge_binding_count", edge_binding_plan.value("summary", json::object()).value("binding_count", 0)},
         {"rate_transition_count",
          rate_transition_plan.value("summary", json::object()).value("transition_count", 0)},
         {"cross_rate_transition_count",
          rate_transition_plan.value("summary", json::object()).value("cross_rate_transition_count", 0)},
         {"runtime_rate_transition_count",
          rate_transition_plan.value("summary", json::object()).value("runtime_transition_count", 0)},
         {"time_plan_node_count", time_plan_summary.value("node_count", 0)},
         {"time_plan_distinct_delta_t_s",
          time_plan_summary.value("distinct_delta_t_s", json::array())},
         {"time_plan_profile_schedule_override_node_count",
          time_plan_summary.value("profile_schedule_override_node_count", 0)},
         {"scheduler_profile_schedule_override_node_count",
          scheduler_plan_summary.value("profile_schedule_override_node_count", 0)},
         {"scheduler_table_dispatch_entry_count",
          scheduler_table_summary.value("dispatch_entry_count", 0)},
         {"scheduler_table_transition_entry_count",
          scheduler_table_summary.value("transition_entry_count", 0)},
         {"scheduler_table_runtime_transition_entry_count",
          scheduler_table_summary.value("runtime_transition_entry_count", 0)},
         {"scheduler_table_profile_schedule_override_entry_count",
          scheduler_table_summary.value("profile_schedule_override_entry_count", 0)},
         {"scheduler_due_time_trace_dispatch_event_count",
          scheduler_due_time_trace_summary.value("dispatch_event_count", 0)},
         {"scheduler_due_time_trace_held_not_due_event_count",
          scheduler_due_time_trace_summary.value("held_not_due_event_count", 0)},
         {"scheduler_due_time_trace_not_due_violation_count",
          scheduler_due_time_trace_summary.value("not_due_violation_count", 0)},
         {"scheduler_due_time_trace_dependency_violation_count",
          scheduler_due_time_trace_summary.value("dependency_violation_count", 0)},
         {"scheduler_due_time_trace_digest",
          scheduler_due_time_trace_summary.value("trace_digest", std::string())},
         {"scheduler_diagnostics_digest",
          scheduler_diagnostics_summary.value("diagnostics_digest", std::string())},
         {"rate_graph_node_count",
          scheduler_diagnostics_summary.value("node_count", 0)},
         {"rate_graph_edge_count",
          scheduler_diagnostics_summary.value("rate_graph_edge_count", 0)},
         {"rate_graph_transition_edge_count",
          scheduler_diagnostics_summary.value("transition_edge_count", 0)},
         {"rate_graph_cross_rate_edge_count",
          scheduler_diagnostics_summary.value("cross_rate_edge_count", 0)},
         {"rate_graph_distinct_period_count",
          scheduler_diagnostics_summary.value("distinct_period_count", 0)},
         {"deterministic_multitasking_batch_count",
          scheduler_diagnostics_summary.value("multitasking_batch_count", 0)},
         {"deterministic_multitasking_parallelizable_batch_count",
          scheduler_diagnostics_summary.value("parallelizable_batch_count", 0)},
         {"deterministic_multitasking_dependency_violation_count",
          scheduler_diagnostics_summary.value("multitasking_dependency_violation_count", 0)},
         {"deadline_check_event_count",
          scheduler_diagnostics_summary.value("deadline_check_event_count", 0)},
         {"deadline_miss_count",
          scheduler_diagnostics_summary.value("deadline_miss_count", 0)},
         {"overrun_count",
          scheduler_diagnostics_summary.value("overrun_count", 0)},
         {"jitter_violation_count",
          scheduler_diagnostics_summary.value("jitter_violation_count", 0)},
         {"max_abs_jitter_s",
          scheduler_diagnostics_summary.value("max_abs_jitter_s", 0.0)},
         {"ready_queue_plan_node_count", pdk_scheduler.plan_nodes.size()},
         {"worker_pool_size", pdk_executor.options.max_workers},
         {"pdk_workflow_process_spawned", false},
         {"operator_process_spawned_by_host", false}}},
       {"refs",
        {{"adapter_lifecycle_log", "adapter_lifecycle_log.json"},
         {"adapter_session_summary", "adapter_session_summary.json"},
         {"adapter_backend_capability_report", "adapter_backend_capability_report.json"},
         {"runtime_node_snapshot", "runtime_node_snapshot.json"},
         {"runtime_outputs", "runtime_outputs.json"},
         {"runtime_loop_summary", "runtime_loop_summary.json"},
         {"runtime_rate_transition_nodes", "runtime_rate_transition_nodes.json"},
         {"edge_binding_plan", "edge_binding_plan.json"},
         {"rate_transition_plan", "rate_transition_plan.json"},
         {"scheduler_table", scheduler_table.empty() ? "" : "scheduler_table.json"},
         {"scheduler_due_time_trace",
          scheduler_due_time_trace.empty() ? "" : "scheduler_due_time_trace.json"},
         {"scheduler_diagnostics",
          scheduler_diagnostics.empty() ? "" : "scheduler_diagnostics.json"},
         {"data_plane_manifest", "data_plane_manifest.json"},
         {"forecast_uncertainty_evidence", "forecast_uncertainty_evidence.json"},
         {"state_checkpoint", "state_checkpoint.json"},
         {"sensor_stream", "sensor_stream.json"}}}});
}

}  // namespace FlightEnvPlatformRuntime
