#include "FlightEnvPlatformRuntime/estimation/SampleScheduler.hpp"

#include "FlightEnvPlatform/Adapter/AdapterAbi.hpp"
#include "FlightEnvPlatform/Runtime/ThreadSafePortStore.hpp"
#include "FlightEnvPlatformRuntime/RuntimeReadyQueueExecutor.hpp"
#include "FlightEnvPlatformRuntime/RuntimeTypedBufferStore.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeEventQueue.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeTimeTypes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace FlightEnvPlatformRuntime::estimation {
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

bool jsonBool(const nlohmann::json& value, const std::string& key, bool fallback = false) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean()) {
    return fallback;
  }
  return value.at(key).get<bool>();
}

std::uint64_t fnv64(const std::string& text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

void appendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    bytes.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
  }
}

void appendI64(std::vector<std::uint8_t>& bytes, std::int64_t value) {
  appendU64(bytes, static_cast<std::uint64_t>(value));
}

std::vector<std::uint8_t> sampleBatchDescriptorBytes(
    const RuntimeSampleSchedulingRequest& request,
    const std::string& operation_kind,
    int sample_start,
    int batch_sample_count) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(64);
  appendU64(bytes, 0x4653455354424131ull);  // "FSESTBA1" little-endian marker.
  appendI64(bytes, request.frame_index);
  appendI64(bytes, RuntimeTimePoint::fromSeconds(request.sample_time_s).nanoseconds);
  appendI64(bytes, sample_start);
  appendI64(bytes, batch_sample_count);
  appendI64(bytes, request.state_dim);
  appendU64(bytes, fnv64(request.sample_kind));
  appendU64(bytes, fnv64(operation_kind));
  return bytes;
}

nlohmann::json resolveSampleSchedulingConfig(const nlohmann::json& estimation_system) {
  nlohmann::json config = estimation_system.value("sample_scheduling", nlohmann::json::object());
  if (!config.is_object()) {
    config = nlohmann::json::object();
  }
  if (!config.contains("enabled")) {
    config["enabled"] = true;
  }
  if (!config.contains("batch_size")) {
    config["batch_size"] = 32;
  }
  if (!config.contains("max_parallel_batches")) {
    config["max_parallel_batches"] = 1;
  }
  if (!config.contains("resource_lock_mode")) {
    config["resource_lock_mode"] = "shared";
  }
  if (!config.contains("parallel_group_id")) {
    config["parallel_group_id"] = "estimation_samples";
  }
  if (!config.contains("capacity_group")) {
    config["capacity_group"] = "estimation_samples";
  }
  if (!config.contains("typed_buffer_mode")) {
    config["typed_buffer_mode"] = "memory_only";
  }
  if (!config.contains("record_batch_evidence")) {
    config["record_batch_evidence"] = true;
  }
  return config;
}

nlohmann::json schedulerPlanWithSampleLimits(nlohmann::json scheduler_plan, const nlohmann::json& config) {
  if (!scheduler_plan.is_object()) {
    scheduler_plan = nlohmann::json::object();
  }
  if (!scheduler_plan.contains("scheduler_policy") || !scheduler_plan["scheduler_policy"].is_object()) {
    scheduler_plan["scheduler_policy"] = nlohmann::json::object();
  }
  nlohmann::json& policy = scheduler_plan["scheduler_policy"];
  const int max_parallel_batches = std::max(1, jsonInt(config, "max_parallel_batches", 1));
  if (!policy.contains("max_parallelism")) {
    policy["max_parallelism"] = max_parallel_batches;
  }
  if (!policy.contains("ready_queue_policy")) {
    policy["ready_queue_policy"] = "time_then_dependency";
  }
  if (!policy.contains("resource_conflict_policy")) {
    policy["resource_conflict_policy"] = "respect_locks";
  }
  if (!policy.contains("deadline_policy")) {
    policy["deadline_policy"] = "mark_stale";
  }
  if (!policy.contains("record_timeline")) {
    policy["record_timeline"] = true;
  }
  if (!policy.contains("capacity_group_limits") || !policy["capacity_group_limits"].is_object()) {
    policy["capacity_group_limits"] = nlohmann::json::object();
  }
  policy["capacity_group_limits"][jsonString(config, "capacity_group", "estimation_samples")] =
      max_parallel_batches;
  return scheduler_plan;
}

std::string operationNodeId(
    const RuntimeSampleSchedulingRequest& request,
    const std::string& operation_kind,
    int batch_index) {
  std::ostringstream oss;
  oss << request.stage_id << ".sample_batch."
      << operation_kind << ".f" << request.frame_index
      << ".b" << batch_index;
  return oss.str();
}

RuntimeTypedBufferAllocation allocateBatchBuffer(
    const RuntimeSampleSchedulingRequest& request,
    const std::string& operation_kind,
    const std::string& port_id,
    int sample_start,
    int batch_sample_count,
    RuntimeTypedBufferPersistenceMode persistence_mode) {
  RuntimeTypedBufferRequest buffer_request;
  buffer_request.run_dir = request.run_dir;
  buffer_request.node_id = operationNodeId(request, operation_kind, sample_start);
  buffer_request.port_id = port_id;
  buffer_request.schema_id = "platform.estimation.sample_batch_descriptor.v1";
  buffer_request.dto_name = "RuntimeSampleBatchDescriptor";
  buffer_request.layout_id = "platform.estimation.sample_batch_descriptor.binary.v1";
  buffer_request.format = "fe_sample_batch_descriptor.v1";
  buffer_request.dtype = flightenv::platform::AdapterScalarDType::UInt8;
  buffer_request.rank = 1;
  buffer_request.bytes = sampleBatchDescriptorBytes(request, operation_kind, sample_start, batch_sample_count);
  buffer_request.shape = {};
  buffer_request.shape[0] = static_cast<std::uint64_t>(buffer_request.bytes.size());
  buffer_request.flags = flightenv::platform::AdapterTypedBufferFlagRuntimeOwned;
  buffer_request.persistence_mode = persistence_mode;
  buffer_request.write_shadow_artifact = runtimeTypedBufferPersistenceWritesShadow(persistence_mode);
  buffer_request.async_shadow_artifact = false;
  return RuntimeTypedBufferStore::instance().allocate(std::move(buffer_request));
}

RuntimeEvent sampleBatchEvent(
    const RuntimeSampleSchedulingRequest& request,
    const std::string& operation_kind,
    int batch_index,
    int sample_start,
    int batch_sample_count,
    const nlohmann::json& input_ref,
    const nlohmann::json& output_ref) {
  RuntimeEvent event;
  event.event_kind = "sample_batch";
  event.event_time = RuntimeTimePoint::fromSeconds(request.sample_time_s);
  event.event_time_s = event.event_time.seconds();
  event.priority = 25000;
  event.iteration_index = request.frame_index;
  event.target_id = operationNodeId(request, operation_kind, batch_index);
  event.payload = {
      {"stage_id", request.stage_id},
      {"operation_kind", operation_kind},
      {"sample_kind", request.sample_kind},
      {"sample_start", sample_start},
      {"sample_count", batch_sample_count},
      {"state_dim", request.state_dim},
      {"typed_input_ref", input_ref},
      {"typed_output_ref", output_ref},
  };
  std::ostringstream oss;
  oss << "evt.runtime.sample_batch."
      << request.stage_id << "." << operation_kind
      << ".f" << request.frame_index << ".b" << batch_index;
  event.event_id = oss.str();
  return event;
}

nlohmann::json sampleBatchNode(
    const RuntimeSampleSchedulingRequest& request,
    const nlohmann::json& config,
    const std::string& operation_kind,
    int batch_index) {
  return {
      {"node_id", operationNodeId(request, operation_kind, batch_index)},
      {"operator_id", "platform.estimation.sample_batch"},
      {"depends_on", nlohmann::json::array()},
      {"resource_refs", nlohmann::json::array({"platform.estimation.sample_batch." + operation_kind})},
      {"parallel_group_id", jsonString(config, "parallel_group_id", "estimation_samples")},
      {"capacity_group", jsonString(config, "capacity_group", "estimation_samples")},
      {"resource_lock_mode", jsonString(config, "resource_lock_mode", "shared")},
      {"can_run_parallel", true},
      {"scheduling_level", 0},
      {"timeout_s", 0.0},
  };
}

}  // namespace

nlohmann::json SampleScheduler::scheduleFrame(const RuntimeSampleSchedulingRequest& request) const {
  const nlohmann::json config = resolveSampleSchedulingConfig(request.estimation_system);
  const bool enabled = jsonBool(config, "enabled", true);
  const int batch_size = std::max(1, jsonInt(config, "batch_size", 32));
  const int sample_count = std::max(0, request.sample_count);
  const RuntimeTypedBufferPersistenceMode persistence_mode =
      resolveRuntimeTypedBufferPersistence(jsonString(config, "typed_buffer_mode", "memory_only"));

  nlohmann::json frame_summary = {
      {"enabled", enabled},
      {"frame_index", request.frame_index},
      {"sample_time_s", request.sample_time_s},
      {"method", request.method_kind},
      {"sample_kind", request.sample_kind},
      {"sample_count", sample_count},
      {"batch_size", batch_size},
      {"accepted_batch_count", 0},
      {"failed_batch_count", 0},
      {"total_batch_count", 0},
      {"typed_buffer_mode", runtimeTypedBufferPersistenceName(persistence_mode)},
      {"config", config},
      {"operations", nlohmann::json::array()},
      {"batches", nlohmann::json::array()},
  };
  if (!enabled || sample_count <= 0) {
    return frame_summary;
  }

  RuntimeReadyQueueExecutor ready_queue =
      RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlanWithSampleLimits(request.scheduler_plan, config));
  flightenv::platform::ThreadSafePortStore port_store;
  const nlohmann::json outputs = nlohmann::json::object();
  const bool record_batches = jsonBool(config, "record_batch_evidence", true);
  int global_batch_index = 0;

  for (const std::string& operation_kind : {"state_transition", "observation_equation"}) {
    int operation_batch_count = 0;
    int operation_accepted_count = 0;
    int operation_failed_count = 0;
    nlohmann::json operation_summary = {
        {"operation_kind", operation_kind},
        {"batch_count", 0},
        {"accepted_batch_count", 0},
        {"failed_batch_count", 0},
    };
    for (int sample_start = 0; sample_start < sample_count; sample_start += batch_size) {
      const int current_batch_size = std::min(batch_size, sample_count - sample_start);
      const RuntimeTypedBufferAllocation input_buffer =
          allocateBatchBuffer(request, operation_kind, "sample_batch.input", sample_start, current_batch_size, persistence_mode);
      const RuntimeTypedBufferAllocation output_buffer =
          allocateBatchBuffer(request, operation_kind, "sample_batch.output", sample_start, current_batch_size, persistence_mode);
      RuntimeEvent event = sampleBatchEvent(
          request,
          operation_kind,
          global_batch_index,
          sample_start,
          current_batch_size,
          input_buffer.ref,
          output_buffer.ref);
      const nlohmann::json node = sampleBatchNode(request, config, operation_kind, global_batch_index);
      const RuntimeReadyAdmission admission =
          ready_queue.admitNode(node, event, outputs, port_store, request.branch_id, request.timeline_id);
      if (admission.accepted) {
        ready_queue.completeNode(admission);
        ++operation_accepted_count;
      } else {
        ++operation_failed_count;
      }
      ++operation_batch_count;
      if (record_batches) {
        frame_summary["batches"].push_back({
            {"batch_index", global_batch_index},
            {"operation_kind", operation_kind},
            {"sample_kind", request.sample_kind},
            {"sample_start", sample_start},
            {"sample_count", current_batch_size},
            {"event", RuntimeEventQueue::eventEvidence(event)},
            {"ready_queue_admission", admission.evidence},
            {"typed_input_ref", input_buffer.ref},
            {"typed_output_ref", output_buffer.ref},
        });
      }
      ++global_batch_index;
    }
    operation_summary["batch_count"] = operation_batch_count;
    operation_summary["accepted_batch_count"] = operation_accepted_count;
    operation_summary["failed_batch_count"] = operation_failed_count;
    frame_summary["total_batch_count"] = frame_summary.value("total_batch_count", 0) + operation_batch_count;
    frame_summary["accepted_batch_count"] = frame_summary.value("accepted_batch_count", 0) + operation_accepted_count;
    frame_summary["failed_batch_count"] = frame_summary.value("failed_batch_count", 0) + operation_failed_count;
    frame_summary["operations"].push_back(operation_summary);
  }
  frame_summary["ready_queue"] = ready_queue.runtimeCoreEvidence();
  return frame_summary;
}

}  // namespace FlightEnvPlatformRuntime::estimation
