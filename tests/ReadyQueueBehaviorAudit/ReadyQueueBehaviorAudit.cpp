#include "FlightEnvPlatformRuntime/RuntimeReadyQueueExecutor.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using FlightEnvPlatformRuntime::RuntimeEvent;
using FlightEnvPlatformRuntime::RuntimeReadyAdmission;
using FlightEnvPlatformRuntime::RuntimeReadyQueueExecutor;
using FlightEnvPlatformRuntime::RuntimeTimePoint;
using json = nlohmann::json;

constexpr std::int64_t kNs = 1000000000LL;

struct AuditContext {
  json cases = json::array();
  int failures = 0;

  void record(const std::string& name, bool passed, const std::string& message, json evidence = json::object()) {
    cases.push_back({
        {"name", name},
        {"status", passed ? "PASS" : "FAIL"},
        {"message", message},
        {"evidence", std::move(evidence)},
    });
    if (!passed) {
      ++failures;
      std::cerr << "[FAIL] " << name << ": " << message << "\n";
    } else {
      std::cout << "[PASS] " << name << "\n";
    }
  }
};

json schedulerPlan(int max_parallelism = 2, const std::string& deadline_policy = "mark_stale") {
  return {
      {"scheduler_policy",
       {
           {"max_parallelism", max_parallelism},
           {"ready_queue_policy", "time_then_dependency"},
           {"resource_conflict_policy", "respect_locks"},
           {"deadline_policy", deadline_policy},
           {"default_deadline_s", 0.0},
           {"record_timeline", true},
       }},
      {"nodes", json::array()},
  };
}

json schedulerPlanWithCapacityLimit(const std::string& group_id, int limit) {
  json plan = schedulerPlan(4);
  plan["scheduler_policy"]["capacity_group_limits"] = {{group_id, limit}};
  return plan;
}

json nodeSpec(const std::string& node_id) {
  return {
      {"node_id", node_id},
      {"operator_id", "op." + node_id},
      {"depends_on", json::array()},
      {"scheduling_level", 0},
      {"parallel_group_id", ""},
      {"deadline_time_s", 0.0},
      {"timeout_s", 0.0},
      {"resource_lock_mode", "shared"},
      {"capacity_group", "default"},
      {"can_run_parallel", true},
  };
}

RuntimeEvent nodeDueEvent(
    const std::string& node_id,
    std::int64_t event_time_ns,
    std::int64_t scheduled_time_ns = -1) {
  RuntimeEvent event;
  event.event_id = "evt.audit.node_due." + node_id + "." + std::to_string(event_time_ns);
  event.event_kind = "node_due";
  event.target_id = node_id;
  event.event_time = RuntimeTimePoint::fromNanoseconds(event_time_ns);
  event.event_time_s = event.event_time.seconds();
  event.priority = 0;
  event.iteration_index = 0;
  if (scheduled_time_ns >= 0) {
    event.payload["scheduled_event_time_ns"] = scheduled_time_ns;
  }
  return event;
}

flightenv::platform::RuntimePacket scopedPacket(
    const std::string& node_id,
    const std::string& port_id,
    const std::string& branch_id,
    const std::string& timeline_id,
    std::int64_t time_ns) {
  flightenv::platform::RuntimePacket packet;
  packet.run_id = "ready_queue_audit";
  packet.object_id = "object.audit";
  packet.port_name = node_id + "." + port_id;
  packet.node_id = node_id;
  packet.time_point.stamp_ns = time_ns;
  packet.time_point.run_time_s = static_cast<double>(time_ns) / static_cast<double>(kNs);
  packet.inline_payload_json = "{}";
  packet.tags["branch_id"] = branch_id;
  packet.tags["timeline_id"] = timeline_id;
  packet.tags["port_id"] = port_id;
  return packet;
}

bool hasString(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool hasEvidenceSection(const RuntimeReadyAdmission& admission, const std::string& key) {
  return admission.evidence.is_object() && admission.evidence.contains(key);
}

void caseAcceptedNode(AuditContext& audit) {
  RuntimeReadyQueueExecutor executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlan());
  flightenv::platform::ThreadSafePortStore port_store;
  const json outputs = json::object();

  const RuntimeReadyAdmission admission =
      executor.admitNode(nodeSpec("A"), nodeDueEvent("A", 1 * kNs), outputs, port_store);

  const bool ok = admission.accepted && admission.status == "ready" && admission.reason == "accepted" &&
                  hasEvidenceSection(admission, "dependency_check") &&
                  hasEvidenceSection(admission, "parallel_check");
  audit.record("accepted_node_records_ready_queue_evidence", ok, admission.reason, admission.evidence);
  executor.completeNode(admission);
}

void caseMissingDependency(AuditContext& audit) {
  RuntimeReadyQueueExecutor executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlan());
  flightenv::platform::ThreadSafePortStore port_store;
  json node = nodeSpec("B");
  node["depends_on"] = json::array({"A"});

  const RuntimeReadyAdmission admission =
      executor.admitNode(node, nodeDueEvent("B", 1 * kNs), json::object(), port_store);

  const bool ok = !admission.accepted && admission.reason == "dependencies_not_ready" &&
                  hasString(admission.missing_dependencies, "A");
  audit.record("missing_dependency_blocks_node", ok, admission.reason, admission.evidence);
}

void caseMissingInputPort(AuditContext& audit) {
  RuntimeReadyQueueExecutor executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlan());
  flightenv::platform::ThreadSafePortStore port_store;
  json node = nodeSpec("C");
  node["edge_bindings"] = json::array({
      {
          {"source_node_id", "A"},
          {"source_port_id", "state.next"},
          {"target_port_id", "state.current"},
      },
  });

  const RuntimeReadyAdmission admission =
      executor.admitNode(node, nodeDueEvent("C", 2 * kNs), json::object(), port_store, "main", "public");

  const bool ok = !admission.accepted && admission.reason == "input_ports_not_ready" &&
                  hasString(admission.missing_input_ports, "state.current");
  audit.record("missing_explicit_input_port_blocks_node", ok, admission.reason, admission.evidence);
}

void casePortStoreReadiness(AuditContext& audit) {
  RuntimeReadyQueueExecutor executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlan());
  flightenv::platform::ThreadSafePortStore port_store;
  port_store.write(scopedPacket("A", "state.next", "main", "public", 1 * kNs));
  json node = nodeSpec("C");
  node["edge_bindings"] = json::array({
      {
          {"source_node_id", "A"},
          {"source_port_id", "state.next"},
          {"target_port_id", "state.current"},
      },
  });

  const RuntimeReadyAdmission admission =
      executor.admitNode(node, nodeDueEvent("C", 2 * kNs), json::object(), port_store, "main", "public");

  const bool ok = admission.accepted && admission.reason == "accepted";
  audit.record("scoped_port_store_packet_satisfies_input_port", ok, admission.reason, admission.evidence);
  executor.completeNode(admission);
}

void caseResourceLock(AuditContext& audit) {
  RuntimeReadyQueueExecutor executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlan(2));
  flightenv::platform::ThreadSafePortStore port_store;
  json first = nodeSpec("R1");
  first["resource_refs"] = json::array({"resource.alpha"});
  first["resource_lock_mode"] = "exclusive";
  json second = nodeSpec("R2");
  second["resource_refs"] = json::array({"resource.alpha"});
  second["resource_lock_mode"] = "exclusive";

  const RuntimeReadyAdmission first_admission =
      executor.admitNode(first, nodeDueEvent("R1", 1 * kNs), json::object(), port_store);
  const RuntimeReadyAdmission blocked =
      executor.admitNode(second, nodeDueEvent("R2", 1 * kNs), json::object(), port_store);
  executor.completeNode(first_admission);
  const RuntimeReadyAdmission after_release =
      executor.admitNode(second, nodeDueEvent("R2", 2 * kNs), json::object(), port_store);

  const bool ok = first_admission.accepted && !blocked.accepted &&
                  blocked.reason == "resource_locked" && after_release.accepted;
  audit.record("exclusive_resource_lock_blocks_and_releases", ok, blocked.reason, blocked.evidence);
  executor.completeNode(after_release);
}

void caseMaxParallelism(AuditContext& audit) {
  RuntimeReadyQueueExecutor executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlan(1));
  flightenv::platform::ThreadSafePortStore port_store;
  const RuntimeReadyAdmission first =
      executor.admitNode(nodeSpec("P1"), nodeDueEvent("P1", 1 * kNs), json::object(), port_store);
  const RuntimeReadyAdmission second =
      executor.admitNode(nodeSpec("P2"), nodeDueEvent("P2", 1 * kNs), json::object(), port_store);

  const bool ok = first.accepted && !second.accepted && second.reason == "max_parallelism_reached";
  audit.record("max_parallelism_blocks_additional_node", ok, second.reason, second.evidence);
  executor.completeNode(first);
}

void caseCapacityGroup(AuditContext& audit) {
  RuntimeReadyQueueExecutor executor =
      RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlanWithCapacityLimit("slow", 1));
  flightenv::platform::ThreadSafePortStore port_store;
  json first = nodeSpec("G1");
  first["capacity_group"] = "slow";
  json second = nodeSpec("G2");
  second["capacity_group"] = "slow";

  const RuntimeReadyAdmission first_admission =
      executor.admitNode(first, nodeDueEvent("G1", 1 * kNs), json::object(), port_store);
  const RuntimeReadyAdmission blocked =
      executor.admitNode(second, nodeDueEvent("G2", 1 * kNs), json::object(), port_store);

  const bool ok = first_admission.accepted && !blocked.accepted &&
                  blocked.reason == "capacity_group_saturated" &&
                  blocked.capacity_group_limit == 1;
  audit.record("capacity_group_limit_blocks_group_saturation", ok, blocked.reason, blocked.evidence);
  executor.completeNode(first_admission);
}

void caseExclusiveDispatch(AuditContext& audit) {
  RuntimeReadyQueueExecutor executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlan(2));
  flightenv::platform::ThreadSafePortStore port_store;
  const RuntimeReadyAdmission first =
      executor.admitNode(nodeSpec("E1"), nodeDueEvent("E1", 1 * kNs), json::object(), port_store);
  json exclusive = nodeSpec("E2");
  exclusive["can_run_parallel"] = false;
  const RuntimeReadyAdmission blocked =
      executor.admitNode(exclusive, nodeDueEvent("E2", 1 * kNs), json::object(), port_store);

  const bool ok = first.accepted && !blocked.accepted &&
                  blocked.reason == "node_requires_exclusive_dispatch";
  audit.record("exclusive_dispatch_node_waits_for_empty_executor", ok, blocked.reason, blocked.evidence);
  executor.completeNode(first);
}

void caseDeadlineMarkStale(AuditContext& audit) {
  json plan = schedulerPlan(2, "mark_stale");
  plan["scheduler_policy"]["default_deadline_s"] = 1.0;
  RuntimeReadyQueueExecutor executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(plan);
  flightenv::platform::ThreadSafePortStore port_store;

  const RuntimeReadyAdmission admission =
      executor.admitNode(nodeSpec("D1"), nodeDueEvent("D1", 3 * kNs, 0), json::object(), port_store);

  const bool ok = !admission.accepted && admission.reason == "deadline_missed" &&
                  admission.status == "stale" && admission.deadline_ns == 1 * kNs &&
                  admission.lateness_ns == 3 * kNs;
  audit.record("deadline_mark_stale_yields_stale_status", ok, admission.status, admission.evidence);
}

void caseDeadlineFail(AuditContext& audit) {
  json plan = schedulerPlan(2, "fail");
  plan["scheduler_policy"]["default_deadline_s"] = 1.0;
  RuntimeReadyQueueExecutor executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(plan);
  flightenv::platform::ThreadSafePortStore port_store;

  const RuntimeReadyAdmission admission =
      executor.admitNode(nodeSpec("D2"), nodeDueEvent("D2", 3 * kNs, 0), json::object(), port_store);

  const bool ok = !admission.accepted && admission.reason == "deadline_missed" &&
                  admission.status == "blocked";
  audit.record("deadline_fail_yields_blocked_status", ok, admission.status, admission.evidence);
}

}  // namespace

int main(int argc, char** argv) {
  AuditContext audit;
  caseAcceptedNode(audit);
  caseMissingDependency(audit);
  caseMissingInputPort(audit);
  casePortStoreReadiness(audit);
  caseResourceLock(audit);
  caseMaxParallelism(audit);
  caseCapacityGroup(audit);
  caseExclusiveDispatch(audit);
  caseDeadlineMarkStale(audit);
  caseDeadlineFail(audit);

  const json report = {
      {"schema_version", "flightenv.platform.ready_queue_behavior_audit.v1"},
      {"result", audit.failures == 0 ? "pass" : "fail"},
      {"case_count", audit.cases.size()},
      {"failure_count", audit.failures},
      {"cases", audit.cases},
  };

  if (argc > 1 && argv[1] != nullptr && std::string(argv[1]).size() > 0) {
    std::ofstream out(argv[1], std::ios::binary);
    out << report.dump(2);
  } else {
    std::cout << report.dump(2) << "\n";
  }

  return audit.failures == 0 ? 0 : 1;
}
