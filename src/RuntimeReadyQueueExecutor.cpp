#include "FlightEnvPlatformRuntime/RuntimeReadyQueueExecutor.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace FlightEnvPlatformRuntime {
namespace {

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

std::int64_t jsonInt64(const nlohmann::json& value, const std::string& key, std::int64_t fallback = 0) {
  if (!value.is_object() || !value.contains(key)) {
    return fallback;
  }
  const auto& item = value.at(key);
  if (item.is_number_integer()) {
    return item.get<std::int64_t>();
  }
  if (item.is_number()) {
    return static_cast<std::int64_t>(item.get<double>());
  }
  return fallback;
}

bool jsonBool(const nlohmann::json& value, const std::string& key, bool fallback = false) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean()) {
    return fallback;
  }
  return value.at(key).get<bool>();
}

flightenv::platform::SimulationTimePoint simulationTimePointFromJson(const nlohmann::json& value) {
  flightenv::platform::SimulationTimePoint point;
  point.run_time_s = jsonDouble(value, "run_time_s", 0.0);
  point.tick_index = jsonInt(value, "tick_index", 0);
  point.source_time_s = jsonDouble(value, "source_time_s", point.run_time_s);
  point.stamp_ns = static_cast<long long>(jsonDouble(value, "stamp_ns", 0.0));
  return point;
}

std::vector<std::string> stringArray(const nlohmann::json& value) {
  std::vector<std::string> result;
  if (!value.is_array()) {
    return result;
  }
  for (const auto& item : value) {
    if (item.is_string()) {
      const std::string text = item.get<std::string>();
      if (!text.empty() && std::find(result.begin(), result.end(), text) == result.end()) {
        result.push_back(text);
      }
    }
  }
  return result;
}

void appendUnique(std::vector<std::string>& target, const std::string& value) {
  if (!value.empty() && std::find(target.begin(), target.end(), value) == target.end()) {
    target.push_back(value);
  }
}

std::vector<std::string> mergedStringArray(
    const nlohmann::json& first,
    const nlohmann::json& second,
    const std::string& key) {
  std::vector<std::string> result = stringArray(first.value(key, nlohmann::json::array()));
  for (const std::string& item : stringArray(second.value(key, nlohmann::json::array()))) {
    appendUnique(result, item);
  }
  return result;
}

nlohmann::json jsonForStrings(const std::vector<std::string>& values) {
  nlohmann::json result = nlohmann::json::array();
  for (const auto& value : values) {
    result.push_back(value);
  }
  return result;
}

struct PortReadiness {
  bool ready = false;
  std::string source;
};

std::string bindingSourcePortId(const nlohmann::json& binding) {
  return jsonString(
      binding,
      "source_port_id",
      jsonString(binding, "source_port", jsonString(binding, "source_port_name")));
}

std::string bindingTargetPortId(const nlohmann::json& binding) {
  return jsonString(
      binding,
      "target_port_id",
      jsonString(binding, "target_port", jsonString(binding, "target_port_name")));
}

nlohmann::json rateTransitionEvidence(const nlohmann::json& binding) {
  if (!binding.is_object() || !binding.contains("rate_transition") ||
      !binding.at("rate_transition").is_object()) {
    return nlohmann::json::object();
  }
  const nlohmann::json& transition = binding.at("rate_transition");
  nlohmann::json evidence = {
      {"transition_id", jsonString(transition, "transition_id")},
      {"binding_id", jsonString(transition, "binding_id")},
      {"rate_relation", jsonString(transition, "rate_relation")},
      {"strategy", jsonString(transition, "strategy")},
      {"source_period_s", transition.value("source_period_s", -1.0)},
      {"target_period_s", transition.value("target_period_s", -1.0)},
      {"requires_runtime_transition", jsonBool(transition, "requires_runtime_transition", false)},
  };
  if (transition.contains("max_age_s")) {
    evidence["max_age_s"] = transition.at("max_age_s");
  }
  return evidence;
}

bool currentOutputsContainPort(
    const nlohmann::json& outputs,
    const std::string& node_id,
    const std::string& port_id) {
  if (!outputs.is_object() || node_id.empty() || port_id.empty() || !outputs.contains(node_id)) {
    return false;
  }
  const nlohmann::json& node_output = outputs.at(node_id);
  if (!node_output.is_object()) {
    return false;
  }
  const nlohmann::json output_ports = node_output.value("outputs", nlohmann::json::object());
  return output_ports.is_object() && output_ports.contains(port_id);
}

}  // namespace

RuntimeReadyQueueExecutor RuntimeReadyQueueExecutor::fromSchedulerPlan(
    const nlohmann::json& scheduler_plan) {
  RuntimeReadyQueueExecutor result;
  const nlohmann::json policy = scheduler_plan.value("scheduler_policy", nlohmann::json::object());
  result.scheduler_.policy.max_parallelism = jsonInt(policy, "max_parallelism", 1);
  result.scheduler_.policy.ready_queue_policy =
      jsonString(policy, "ready_queue_policy", "time_then_dependency");
  result.scheduler_.policy.resource_conflict_policy =
      jsonString(policy, "resource_conflict_policy", "respect_locks");
  result.scheduler_.policy.deadline_policy = jsonString(policy, "deadline_policy", "mark_stale");
  result.scheduler_.policy.default_deadline_s = jsonDouble(policy, "default_deadline_s", 0.0);
  result.scheduler_.policy.record_timeline = policy.value("record_timeline", true);
  const nlohmann::json capacity_limits =
      policy.value("capacity_group_limits", nlohmann::json::object());
  if (capacity_limits.is_object()) {
    for (auto it = capacity_limits.begin(); it != capacity_limits.end(); ++it) {
      const int limit = it.value().is_number_integer()
          ? it.value().get<int>()
          : (it.value().is_number() ? static_cast<int>(it.value().get<double>()) : 0);
      if (!it.key().empty() && limit > 0) {
        result.scheduler_.policy.capacity_group_limits[it.key()] = limit;
      }
    }
  }

  const nlohmann::json nodes = scheduler_plan.value("nodes", nlohmann::json::array());
  if (nodes.is_array()) {
    for (const auto& node : nodes) {
      flightenv::platform::SchedulerPlanNode plan_node;
      plan_node.node_id = jsonString(node, "node_id");
      plan_node.operator_id = jsonString(node, "operator_id");
      if (node.contains("depends_on") && node.at("depends_on").is_array()) {
        for (const auto& dep : node.at("depends_on")) {
          if (dep.is_string()) {
            plan_node.depends_on.push_back(dep.get<std::string>());
          }
        }
      }
      plan_node.scheduling_level = jsonInt(node, "scheduling_level", 0);
      plan_node.parallel_group_id = jsonString(node, "parallel_group_id");
      plan_node.ready_time = simulationTimePointFromJson(node.value("ready_time", nlohmann::json::object()));
      plan_node.deadline_time_s = jsonDouble(node, "deadline_time_s", 0.0);
      plan_node.timeout_s = jsonDouble(node, "timeout_s", 0.0);
      plan_node.capacity_group = jsonString(node, "capacity_group");
      plan_node.resource_lock_mode = jsonString(node, "resource_lock_mode");
      plan_node.can_run_parallel = node.value("can_run_parallel", true);
      if (!plan_node.node_id.empty()) {
        result.plan_node_by_id_[plan_node.node_id] = node;
      }
      result.scheduler_.plan_nodes.push_back(std::move(plan_node));
    }
  }

  result.executor_.options.max_workers = result.scheduler_.max_parallelism();
  result.executor_.options.worker_name_prefix = "flightenv-runtime-native";
  return result;
}

const flightenv::platform::ReadyQueueScheduler& RuntimeReadyQueueExecutor::scheduler() const {
  return scheduler_;
}

const flightenv::platform::ThreadPoolExecutorDescriptor& RuntimeReadyQueueExecutor::executor() const {
  return executor_;
}

nlohmann::json RuntimeReadyQueueExecutor::schedulerEvidence() const {
  return {
      {"type", "FlightEnvPlatform::ReadyQueueScheduler"},
      {"policy",
       {
           {"max_parallelism", scheduler_.policy.max_parallelism},
           {"ready_queue_policy", scheduler_.policy.ready_queue_policy},
           {"resource_conflict_policy", scheduler_.policy.resource_conflict_policy},
           {"deadline_policy", scheduler_.policy.deadline_policy},
           {"default_deadline_s", scheduler_.policy.default_deadline_s},
           {"record_timeline", scheduler_.policy.record_timeline},
           {"capacity_group_limits", scheduler_.policy.capacity_group_limits},
       }},
      {"plan_node_count", scheduler_.plan_nodes.size()},
      {"effective_max_parallelism", scheduler_.max_parallelism()},
  };
}

nlohmann::json RuntimeReadyQueueExecutor::executorEvidence() const {
  return {
      {"type", "FlightEnvPlatform::ThreadPoolExecutorDescriptor"},
      {"options",
       {
           {"max_workers", executor_.options.max_workers},
           {"worker_name_prefix", executor_.options.worker_name_prefix},
       }},
  };
}

nlohmann::json RuntimeReadyQueueExecutor::readyQueueStateEvidence() const {
  nlohmann::json active_parallel_groups = nlohmann::json::array();
  for (const auto& group_id : active_parallel_groups_) {
    active_parallel_groups.push_back(group_id);
  }
  return {
      {"active_execution_count", active_execution_count_},
      {"active_resource_locks", active_resource_locks_},
      {"active_parallel_groups", active_parallel_groups},
      {"active_capacity_groups", active_capacity_groups_},
  };
}

nlohmann::json RuntimeReadyQueueExecutor::runtimeCoreEvidence() const {
  return {
      {"ready_queue_scheduler", schedulerEvidence()},
      {"thread_pool_executor", executorEvidence()},
      {"ready_queue_state", readyQueueStateEvidence()},
  };
}

RuntimeReadyAdmission RuntimeReadyQueueExecutor::admitNode(
    const nlohmann::json& node,
    const RuntimeEvent& event,
    const nlohmann::json& outputs,
    const flightenv::platform::ThreadSafePortStore& port_store,
    const std::string& branch_id,
    const std::string& timeline_id) {
  RuntimeReadyAdmission admission;
  admission.node_id = jsonString(node, "node_id");
  admission.operator_id = jsonString(node, "operator_id");
  admission.runtime_event_id = event.event_id;
  admission.runtime_event_kind = event.event_kind;
  admission.status = "blocked";
  admission.reason = "not_checked";

  const auto plan_it = plan_node_by_id_.find(admission.node_id);
  const nlohmann::json plan_node =
      plan_it == plan_node_by_id_.end() ? nlohmann::json::object() : plan_it->second;
  const nlohmann::json schedule_source = plan_node.is_object() && !plan_node.empty() ? plan_node : node;

  admission.parallel_group_id = jsonString(schedule_source, "parallel_group_id");
  admission.capacity_group = jsonString(schedule_source, "capacity_group", "default");
  admission.resource_lock_mode = jsonString(schedule_source, "resource_lock_mode", "shared");
  admission.scheduling_level = jsonInt(schedule_source, "scheduling_level", 0);
  admission.can_run_parallel = jsonBool(schedule_source, "can_run_parallel", true);
  const auto capacity_limit = scheduler_.policy.capacity_group_limits.find(admission.capacity_group);
  admission.capacity_group_limit =
      capacity_limit == scheduler_.policy.capacity_group_limits.end() ? 0 : capacity_limit->second;

  auto node_output_ready = [&](const std::string& upstream_node_id) {
    return outputs.contains(upstream_node_id);
  };

  auto source_port_ready = [&](const std::string& upstream_node_id, const std::string& source_port_id) {
    PortReadiness readiness;
    if (upstream_node_id.empty()) {
      readiness.source = "missing_source_node";
      return readiness;
    }
    if (source_port_id.empty()) {
      readiness.source = "missing_source_port";
      return readiness;
    }
    if (currentOutputsContainPort(outputs, upstream_node_id, source_port_id)) {
      readiness.ready = true;
      readiness.source = "current_outputs";
      return readiness;
    }
    if (port_store
            .read_node_port_output_at_or_before(
                branch_id,
                timeline_id,
                upstream_node_id,
                source_port_id,
                event.event_time.nanoseconds)
            .has_value()) {
      readiness.ready = true;
      readiness.source = "port_store_scoped_at_or_before";
      return readiness;
    }
    if (port_store
            .read_node_port_output_scoped(branch_id, timeline_id, upstream_node_id, source_port_id)
            .has_value()) {
      readiness.ready = true;
      readiness.source = "port_store_scoped_latest";
      return readiness;
    }
    readiness.source = "missing_scoped_port_packet";
    return readiness;
  };

  const std::vector<std::string> dependencies =
      mergedStringArray(schedule_source, node, "depends_on");
  for (const std::string& dep : dependencies) {
    if (!node_output_ready(dep)) {
      appendUnique(admission.missing_dependencies, dep);
    }
  }

  nlohmann::json port_check_details = nlohmann::json::array();
  if (node.contains("edge_bindings") && node.at("edge_bindings").is_array()) {
    for (const auto& binding : node.at("edge_bindings")) {
      if (!binding.is_object()) {
        continue;
      }
      const std::string source_node_id = jsonString(binding, "source_node_id");
      const std::string source_port_id = bindingSourcePortId(binding);
      const std::string target_port_id = bindingTargetPortId(binding);
      const PortReadiness readiness = source_port_ready(source_node_id, source_port_id);
      nlohmann::json port_check = {
          {"source_node_id", source_node_id},
          {"source_port_id", source_port_id},
          {"target_port_id", target_port_id},
          {"branch_id", branch_id},
          {"timeline_id", timeline_id},
          {"ready", readiness.ready},
          {"readiness_source", readiness.source},
      };
      const nlohmann::json transition_evidence = rateTransitionEvidence(binding);
      if (!transition_evidence.empty()) {
        port_check["rate_transition"] = transition_evidence;
      }
      port_check_details.push_back(port_check);
      if (!readiness.ready) {
        appendUnique(admission.missing_input_ports, target_port_id.empty() ? source_node_id : target_port_id);
      }
    }
  }

  const std::vector<std::string> resource_refs =
      mergedStringArray(schedule_source, node, "resource_refs");
  for (const std::string& resource_id : resource_refs) {
    const auto locked = active_resource_locks_.find(resource_id);
    const bool request_exclusive = admission.resource_lock_mode == "exclusive";
    const bool existing_exclusive = locked != active_resource_locks_.end() && locked->second == "exclusive";
    if (locked != active_resource_locks_.end() && (request_exclusive || existing_exclusive)) {
      appendUnique(admission.blocked_resource_ids, resource_id);
    } else {
      appendUnique(admission.locked_resource_ids, resource_id);
    }
  }

  const int max_parallelism = scheduler_.max_parallelism();
  bool parallel_blocked = false;
  std::string parallel_reason;
  const auto active_capacity = active_capacity_groups_.find(admission.capacity_group);
  const int active_capacity_count =
      active_capacity == active_capacity_groups_.end() ? 0 : active_capacity->second;
  if (max_parallelism > 0 && active_execution_count_ >= max_parallelism) {
    parallel_blocked = true;
    parallel_reason = "max_parallelism_reached";
  } else if (admission.capacity_group_limit > 0 &&
             active_capacity_count >= admission.capacity_group_limit) {
    parallel_blocked = true;
    parallel_reason = "capacity_group_saturated";
  } else if (!admission.can_run_parallel && active_execution_count_ > 0) {
    parallel_blocked = true;
    parallel_reason = "node_requires_exclusive_dispatch";
  } else if (!admission.parallel_group_id.empty() &&
             active_parallel_groups_.count(admission.parallel_group_id) > 0 &&
             !admission.can_run_parallel) {
    parallel_blocked = true;
    parallel_reason = "parallel_group_active";
  }

  const std::int64_t scheduled_time_ns =
      jsonInt64(event.payload, "scheduled_event_time_ns", event.event_time.nanoseconds);
  const double timeout_s = jsonDouble(schedule_source, "timeout_s", 0.0);
  const double deadline_time_s = jsonDouble(schedule_source, "deadline_time_s", 0.0);
  admission.deadline_s = timeout_s > 0.0 ? timeout_s :
      (deadline_time_s > 0.0 ? deadline_time_s : scheduler_.policy.default_deadline_s);
  admission.deadline_ns = RuntimeDuration::fromSeconds(admission.deadline_s).nanoseconds;
  admission.lateness_ns = std::max<std::int64_t>(0, event.event_time.nanoseconds - scheduled_time_ns);
  admission.lateness_s = RuntimeDuration::fromNanoseconds(admission.lateness_ns).seconds();
  const bool deadline_missed = admission.deadline_ns > 0 && admission.lateness_ns > admission.deadline_ns;
  admission.deadline_status = deadline_missed ? "missed" :
      (admission.deadline_ns > 0 ? "within_deadline" : "not_configured");

  if (!admission.missing_dependencies.empty()) {
    admission.reason = "dependencies_not_ready";
  } else if (!admission.missing_input_ports.empty()) {
    admission.reason = "input_ports_not_ready";
  } else if (!admission.blocked_resource_ids.empty()) {
    admission.reason = "resource_locked";
  } else if (parallel_blocked) {
    admission.reason = parallel_reason;
  } else if (deadline_missed) {
    admission.reason = "deadline_missed";
    admission.status = scheduler_.policy.deadline_policy == "mark_stale" ? "stale" : "blocked";
  } else {
    admission.accepted = true;
    admission.status = "ready";
    admission.reason = "accepted";
    ++active_execution_count_;
    if (!admission.parallel_group_id.empty()) {
      active_parallel_groups_.insert(admission.parallel_group_id);
    }
    if (!admission.capacity_group.empty()) {
      ++active_capacity_groups_[admission.capacity_group];
    }
    for (const std::string& resource_id : admission.locked_resource_ids) {
      active_resource_locks_[resource_id] = admission.resource_lock_mode.empty()
          ? std::string("shared")
          : admission.resource_lock_mode;
    }
  }

  admission.evidence = {
      {"node_id", admission.node_id},
      {"operator_id", admission.operator_id},
      {"accepted", admission.accepted},
      {"status", admission.status},
      {"reason", admission.reason},
      {"runtime_event_id", admission.runtime_event_id},
      {"runtime_event_kind", admission.runtime_event_kind},
      {"event_time_s", event.event_time_s},
      {"event_time_ns", event.event_time.nanoseconds},
      {"loop_iteration_index", event.iteration_index},
      {"dependency_check",
       {
           {"required", jsonForStrings(dependencies)},
           {"missing", jsonForStrings(admission.missing_dependencies)},
       }},
      {"port_check",
       {
           {"missing_input_ports", jsonForStrings(admission.missing_input_ports)},
           {"checked_ports", port_check_details},
       }},
      {"resource_check",
       {
           {"resource_lock_mode", admission.resource_lock_mode},
           {"locked_resource_ids", jsonForStrings(admission.locked_resource_ids)},
           {"blocked_resource_ids", jsonForStrings(admission.blocked_resource_ids)},
       }},
      {"parallel_check",
       {
           {"max_parallelism", scheduler_.max_parallelism()},
           {"active_execution_count", active_execution_count_},
           {"can_run_parallel", admission.can_run_parallel},
           {"parallel_group_id", admission.parallel_group_id},
           {"capacity_group", admission.capacity_group},
           {"capacity_group_limit", admission.capacity_group_limit},
           {"active_capacity_group_count", active_capacity_count},
           {"blocked", parallel_blocked},
           {"reason", parallel_reason},
       }},
      {"deadline_check",
       {
           {"deadline_s", admission.deadline_s},
           {"deadline_ns", admission.deadline_ns},
           {"lateness_s", admission.lateness_s},
           {"lateness_ns", admission.lateness_ns},
           {"deadline_status", admission.deadline_status},
           {"deadline_policy", scheduler_.policy.deadline_policy},
       }},
      {"ready_queue_state", readyQueueStateEvidence()},
  };
  return admission;
}

void RuntimeReadyQueueExecutor::completeNode(const RuntimeReadyAdmission& admission) {
  if (!admission.accepted) {
    return;
  }
  if (active_execution_count_ > 0) {
    --active_execution_count_;
  }
  if (!admission.parallel_group_id.empty()) {
    active_parallel_groups_.erase(admission.parallel_group_id);
  }
  if (!admission.capacity_group.empty()) {
    auto found = active_capacity_groups_.find(admission.capacity_group);
    if (found != active_capacity_groups_.end()) {
      if (found->second <= 1) {
        active_capacity_groups_.erase(found);
      } else {
        --found->second;
      }
    }
  }
  for (const std::string& resource_id : admission.locked_resource_ids) {
    active_resource_locks_.erase(resource_id);
  }
}

}  // namespace FlightEnvPlatformRuntime
