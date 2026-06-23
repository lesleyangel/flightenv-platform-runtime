#include "FlightEnvPlatformRuntime/RuntimeReadyQueueExecutor.hpp"
#include "FlightEnvPlatformRuntime/RuntimeFailurePolicy.hpp"
#include "FlightEnvPlatformRuntime/RuntimeZeroCopyPolicy.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeEventQueue.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using FlightEnvPlatformRuntime::RuntimeEvent;
using FlightEnvPlatformRuntime::RuntimeEventQueue;
using FlightEnvPlatformRuntime::RuntimeFailurePolicyRequest;
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
    if (passed) {
      std::cout << "[PASS] " << name << "\n";
    } else {
      ++failures;
      std::cerr << "[FAIL] " << name << ": " << message << "\n";
    }
  }
};

int parseIntArg(const char* value, int fallback) {
  if (value == nullptr) {
    return fallback;
  }
  try {
    const int parsed = std::stoi(value);
    return parsed > 0 ? parsed : fallback;
  } catch (...) {
    return fallback;
  }
}

json schedulerPlan(int max_parallelism = 8, const std::string& deadline_policy = "mark_stale") {
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

json schedulerPlanWithCapacityLimit(int max_parallelism, const std::string& group_id, int limit) {
  json plan = schedulerPlan(max_parallelism);
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

RuntimeEvent stressEvent(
    const std::string& kind,
    std::int64_t event_time_ns,
    int priority,
    int insertion_index) {
  RuntimeEvent event;
  event.event_kind = kind;
  event.event_time = RuntimeTimePoint::fromNanoseconds(event_time_ns);
  event.event_time_s = event.event_time.seconds();
  event.priority = priority;
  event.iteration_index = insertion_index;
  event.target_id = "target." + std::to_string(insertion_index % 127);
  event.payload = {{"insertion_index", insertion_index}};
  return event;
}

flightenv::platform::RuntimePacket scopedPacket(
    const std::string& node_id,
    const std::string& port_id,
    const std::string& branch_id,
    const std::string& timeline_id,
    std::int64_t time_ns) {
  flightenv::platform::RuntimePacket packet;
  packet.run_id = "scheduler_stress_fault_audit";
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

void caseEventQueueStress(AuditContext& audit, int event_count) {
  RuntimeEventQueue queue;
  const std::vector<std::string> kinds = {
      "input_arrived",
      "node_due",
      "public_tick",
      "checkpoint_due",
      "branch_triggered",
      "stop_check_due",
  };
  for (int i = 0; i < event_count; ++i) {
    const std::int64_t time_ns = static_cast<std::int64_t>((i * 37) % 257) * 1234567LL;
    const int priority = (i * 11) % 7;
    queue.push(stressEvent(kinds[static_cast<std::size_t>(i) % kinds.size()], time_ns, priority, i));
  }

  bool ordered = true;
  int popped = 0;
  std::int64_t previous_time = -1;
  int previous_priority = -1;
  int previous_insertion = -1;
  json kind_counts = json::object();
  while (!queue.empty()) {
    const RuntimeEvent event = queue.pop();
    const int insertion = event.payload.value("insertion_index", -1);
    if (previous_time > event.event_time.nanoseconds) {
      ordered = false;
    } else if (previous_time == event.event_time.nanoseconds) {
      if (previous_priority > event.priority) {
        ordered = false;
      } else if (previous_priority == event.priority && previous_insertion > insertion) {
        ordered = false;
      }
    }
    previous_time = event.event_time.nanoseconds;
    previous_priority = event.priority;
    previous_insertion = insertion;
    kind_counts[event.event_kind] = kind_counts.value(event.event_kind, 0) + 1;
    ++popped;
  }

  bool all_kinds_seen = true;
  for (const auto& kind : kinds) {
    all_kinds_seen = all_kinds_seen && kind_counts.value(kind, 0) > 0;
  }
  audit.record(
      "event_queue_orders_many_mixed_events",
      ordered && all_kinds_seen && popped == event_count,
      "event queue must keep time, priority and FIFO ordering under pressure",
      {{"event_count", event_count}, {"popped", popped}, {"kind_counts", kind_counts}});
}

void caseMaxParallelismBackpressure(AuditContext& audit, int max_parallelism) {
  RuntimeReadyQueueExecutor executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlan(max_parallelism));
  flightenv::platform::ThreadSafePortStore port_store;
  std::vector<RuntimeReadyAdmission> active;
  for (int i = 0; i < max_parallelism; ++i) {
    const RuntimeReadyAdmission admission =
        executor.admitNode(nodeSpec("parallel." + std::to_string(i)), nodeDueEvent("parallel", kNs), json::object(), port_store);
    if (admission.accepted) {
      active.push_back(admission);
    }
  }
  const RuntimeReadyAdmission blocked =
      executor.admitNode(nodeSpec("parallel.blocked"), nodeDueEvent("parallel.blocked", kNs), json::object(), port_store);
  for (const RuntimeReadyAdmission& admission : active) {
    executor.completeNode(admission);
  }
  const RuntimeReadyAdmission after_release =
      executor.admitNode(nodeSpec("parallel.after_release"), nodeDueEvent("parallel.after_release", 2 * kNs), json::object(), port_store);

  audit.record(
      "max_parallelism_backpressure_blocks_then_recovers",
      static_cast<int>(active.size()) == max_parallelism &&
          !blocked.accepted &&
          blocked.reason == "max_parallelism_reached" &&
          after_release.accepted,
      blocked.reason,
      {{"max_parallelism", max_parallelism},
       {"accepted_before_block", active.size()},
       {"blocked_reason", blocked.reason},
       {"after_release_reason", after_release.reason}});
  executor.completeNode(after_release);
}

void caseCapacityGroupBackpressure(AuditContext& audit, int group_limit) {
  RuntimeReadyQueueExecutor executor =
      RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlanWithCapacityLimit(group_limit + 4, "field", group_limit));
  flightenv::platform::ThreadSafePortStore port_store;
  std::vector<RuntimeReadyAdmission> active;
  for (int i = 0; i < group_limit; ++i) {
    json node = nodeSpec("capacity." + std::to_string(i));
    node["capacity_group"] = "field";
    const RuntimeReadyAdmission admission =
        executor.admitNode(node, nodeDueEvent("capacity", kNs), json::object(), port_store);
    if (admission.accepted) {
      active.push_back(admission);
    }
  }
  json blocked_node = nodeSpec("capacity.blocked");
  blocked_node["capacity_group"] = "field";
  const RuntimeReadyAdmission blocked =
      executor.admitNode(blocked_node, nodeDueEvent("capacity.blocked", kNs), json::object(), port_store);
  executor.completeNode(active.front());
  const RuntimeReadyAdmission after_release =
      executor.admitNode(blocked_node, nodeDueEvent("capacity.after_release", 2 * kNs), json::object(), port_store);

  for (std::size_t i = 1; i < active.size(); ++i) {
    executor.completeNode(active[i]);
  }
  executor.completeNode(after_release);

  audit.record(
      "capacity_group_backpressure_blocks_then_recovers",
      static_cast<int>(active.size()) == group_limit &&
          !blocked.accepted &&
          blocked.reason == "capacity_group_saturated" &&
          after_release.accepted,
      blocked.reason,
      {{"capacity_group", "field"},
       {"capacity_group_limit", group_limit},
       {"blocked_reason", blocked.reason},
       {"after_release_reason", after_release.reason}});
}

void caseResourceLockStorm(AuditContext& audit, int contenders) {
  RuntimeReadyQueueExecutor executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlan(contenders + 1));
  flightenv::platform::ThreadSafePortStore port_store;
  json owner = nodeSpec("resource.owner");
  owner["resource_refs"] = json::array({"resource.shared"});
  owner["resource_lock_mode"] = "exclusive";
  const RuntimeReadyAdmission accepted =
      executor.admitNode(owner, nodeDueEvent("resource.owner", kNs), json::object(), port_store);

  int blocked_count = 0;
  for (int i = 0; i < contenders; ++i) {
    json contender = nodeSpec("resource.contender." + std::to_string(i));
    contender["resource_refs"] = json::array({"resource.shared"});
    contender["resource_lock_mode"] = "exclusive";
    const RuntimeReadyAdmission blocked =
        executor.admitNode(contender, nodeDueEvent("resource.contender", kNs), json::object(), port_store);
    if (!blocked.accepted && blocked.reason == "resource_locked") {
      ++blocked_count;
    }
  }
  executor.completeNode(accepted);
  json after = nodeSpec("resource.after_release");
  after["resource_refs"] = json::array({"resource.shared"});
  after["resource_lock_mode"] = "exclusive";
  const RuntimeReadyAdmission after_release =
      executor.admitNode(after, nodeDueEvent("resource.after_release", 2 * kNs), json::object(), port_store);

  audit.record(
      "exclusive_resource_lock_storm_blocks_then_recovers",
      accepted.accepted && blocked_count == contenders && after_release.accepted,
      "exclusive resource lock must block contenders and release cleanly",
      {{"contenders", contenders}, {"blocked_count", blocked_count}, {"after_release_reason", after_release.reason}});
  executor.completeNode(after_release);
}

void caseScopedBranchPortStoreStress(AuditContext& audit, int branch_count) {
  RuntimeReadyQueueExecutor executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(schedulerPlan(2));
  flightenv::platform::ThreadSafePortStore port_store;
  json consumer = nodeSpec("consumer");
  consumer["edge_bindings"] = json::array({
      {
          {"source_node_id", "producer"},
          {"source_port_id", "state.next"},
          {"target_port_id", "state.current"},
      },
  });

  for (int i = 0; i < branch_count; ++i) {
    port_store.write(scopedPacket("producer", "state.next", "branch." + std::to_string(i), "public", i * kNs));
  }

  int accepted_count = 0;
  for (int i = 0; i < branch_count; ++i) {
    const RuntimeReadyAdmission admission = executor.admitNode(
        consumer,
        nodeDueEvent("consumer", (i + 1) * kNs),
        json::object(),
        port_store,
        "branch." + std::to_string(i),
        "public");
    if (admission.accepted) {
      ++accepted_count;
      executor.completeNode(admission);
    }
  }
  const RuntimeReadyAdmission wrong_branch = executor.admitNode(
      consumer,
      nodeDueEvent("consumer.wrong", (branch_count + 1) * kNs),
      json::object(),
      port_store,
      "branch.missing",
      "public");

  audit.record(
      "branch_time_scoped_port_store_survives_many_branches",
      accepted_count == branch_count &&
          !wrong_branch.accepted &&
          wrong_branch.reason == "input_ports_not_ready",
      wrong_branch.reason,
      {{"branch_count", branch_count},
       {"accepted_count", accepted_count},
       {"wrong_branch_reason", wrong_branch.reason}});
}

void caseDeadlineFaultPolicies(AuditContext& audit) {
  json fail_plan = schedulerPlan(4, "fail");
  fail_plan["scheduler_policy"]["default_deadline_s"] = 1.0;
  RuntimeReadyQueueExecutor fail_executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(fail_plan);
  flightenv::platform::ThreadSafePortStore port_store;
  const RuntimeReadyAdmission fail_admission =
      fail_executor.admitNode(nodeSpec("deadline.fail"), nodeDueEvent("deadline.fail", 3 * kNs, 0), json::object(), port_store);

  json stale_plan = schedulerPlan(4, "mark_stale");
  stale_plan["scheduler_policy"]["default_deadline_s"] = 1.0;
  RuntimeReadyQueueExecutor stale_executor = RuntimeReadyQueueExecutor::fromSchedulerPlan(stale_plan);
  const RuntimeReadyAdmission stale_admission =
      stale_executor.admitNode(nodeSpec("deadline.stale"), nodeDueEvent("deadline.stale", 3 * kNs, 0), json::object(), port_store);

  audit.record(
      "deadline_fault_policy_fail_and_mark_stale_are_distinct",
      !fail_admission.accepted &&
          fail_admission.reason == "deadline_missed" &&
          fail_admission.status == "blocked" &&
          !stale_admission.accepted &&
          stale_admission.reason == "deadline_missed" &&
          stale_admission.status == "stale",
      "deadline policies must produce stable blocked/stale states",
      {{"fail_status", fail_admission.status},
       {"stale_status", stale_admission.status},
       {"fail_lateness_ns", fail_admission.lateness_ns},
       {"stale_lateness_ns", stale_admission.lateness_ns}});
}

void caseZeroCopyFailFastPolicies(AuditContext& audit) {
  const json data_plane = {
      {"outputs",
       json::array({
           {
               {"port_id", "state.next"},
               {"typed_io_contract",
                {
                    {"json_operator_io_forbidden", true},
                    {"dto_id", "audit.state.v1"},
                }},
           },
       })},
  };
  const json inline_result = {
      {"outputs",
       {
           {"state.next",
            {
                {"contract_id", "audit.state.v1"},
                {"value_kind", "state"},
                {"value", {{"x", 1.0}}},
            }},
       }},
  };
  const json ref_result = {
      {"outputs",
       {
           {"state.next",
            {
                {"contract_id", "audit.state.v1"},
                {"value_kind", "state"},
                {"typed_buffer_ref", {{"buffer_id", "tb.audit"}, {"uri", "runtime://typed-buffer/tb.audit"}}},
            }},
       }},
  };

  bool inline_rejected = false;
  try {
    FlightEnvPlatformRuntime::enforceOutputZeroCopyPolicy(data_plane, inline_result, "zero_copy.inline");
  } catch (const std::exception&) {
    inline_rejected = true;
  }

  bool ref_accepted = true;
  try {
    FlightEnvPlatformRuntime::enforceOutputZeroCopyPolicy(data_plane, ref_result, "zero_copy.ref");
  } catch (const std::exception&) {
    ref_accepted = false;
  }

  const auto unavailable = FlightEnvPlatformRuntime::decideRuntimeZeroCopyExecute(
      data_plane,
      json::object(),
      "require",
      true,
      false,
      "zero_copy.require",
      "adapter.audit");
  const auto available = FlightEnvPlatformRuntime::decideRuntimeZeroCopyExecute(
      data_plane,
      {{"typed_buffer_ref", {{"buffer_id", "tb.input"}}}},
      "require",
      true,
      true,
      "zero_copy.typed",
      "adapter.audit");

  audit.record(
      "typed_zero_copy_fail_fast_blocks_inline_hot_path",
      inline_rejected && ref_accepted && unavailable.fail_fast && available.use_typed_execute,
      "typed-only ports must reject inline JSON and select typed execution when available",
      {{"inline_rejected", inline_rejected},
       {"ref_accepted", ref_accepted},
       {"unavailable", FlightEnvPlatformRuntime::runtimeZeroCopyExecuteDecisionToJson(unavailable)},
      {"available", FlightEnvPlatformRuntime::runtimeZeroCopyExecuteDecisionToJson(available)}});
}

void caseFailureControlPolicies(AuditContext& audit) {
  RuntimeFailurePolicyRequest retry_request;
  retry_request.event_kind = "timeout";
  retry_request.policy = "retry";
  retry_request.attempt_index = 1;
  retry_request.max_retries = 3;

  RuntimeFailurePolicyRequest retry_exhausted = retry_request;
  retry_exhausted.attempt_index = 3;

  RuntimeFailurePolicyRequest degrade_request;
  degrade_request.event_kind = "adapter_failure";
  degrade_request.policy = "degrade";
  degrade_request.can_degrade = true;

  RuntimeFailurePolicyRequest cancel_request;
  cancel_request.event_kind = "cancel_requested";
  cancel_request.policy = "retry";
  cancel_request.attempt_index = 0;
  cancel_request.max_retries = 3;

  RuntimeFailurePolicyRequest backpressure_request;
  backpressure_request.event_kind = "backpressure_saturated";
  backpressure_request.policy = "fail_fast";
  backpressure_request.backpressure_saturated = true;

  const auto retry = FlightEnvPlatformRuntime::decideRuntimeFailurePolicy(retry_request);
  const auto exhausted = FlightEnvPlatformRuntime::decideRuntimeFailurePolicy(retry_exhausted);
  const auto degrade = FlightEnvPlatformRuntime::decideRuntimeFailurePolicy(degrade_request);
  const auto cancel = FlightEnvPlatformRuntime::decideRuntimeFailurePolicy(cancel_request);
  const auto backpressure =
      FlightEnvPlatformRuntime::decideRuntimeFailurePolicy(backpressure_request);

  audit.record(
      "failure_control_retry_cancel_degrade_backpressure_are_distinct",
      retry.retry &&
          !retry.terminal &&
          retry.next_attempt_index == 2 &&
          exhausted.terminal &&
          exhausted.action == "fail" &&
          degrade.degraded &&
          !degrade.terminal &&
          cancel.cancelled &&
          cancel.terminal &&
          backpressure.backpressure_wait &&
          !backpressure.terminal,
      "failure policy service must produce distinct retry/cancel/degrade/backpressure decisions",
      {{"retry", FlightEnvPlatformRuntime::runtimeFailurePolicyDecisionToJson(retry)},
       {"retry_exhausted", FlightEnvPlatformRuntime::runtimeFailurePolicyDecisionToJson(exhausted)},
       {"degrade", FlightEnvPlatformRuntime::runtimeFailurePolicyDecisionToJson(degrade)},
       {"cancel", FlightEnvPlatformRuntime::runtimeFailurePolicyDecisionToJson(cancel)},
       {"backpressure", FlightEnvPlatformRuntime::runtimeFailurePolicyDecisionToJson(backpressure)}});
}

}  // namespace

int main(int argc, char** argv) {
  const std::string report_path = argc > 1 && argv[1] != nullptr ? argv[1] : "";
  const int event_count = argc > 2 ? parseIntArg(argv[2], 10000) : 10000;
  const int branch_count = argc > 3 ? parseIntArg(argv[3], 64) : 64;
  AuditContext audit;

  caseEventQueueStress(audit, std::max(event_count, 10000));
  caseMaxParallelismBackpressure(audit, 16);
  caseCapacityGroupBackpressure(audit, 4);
  caseResourceLockStorm(audit, 64);
  caseScopedBranchPortStoreStress(audit, branch_count);
  caseDeadlineFaultPolicies(audit);
  caseZeroCopyFailFastPolicies(audit);
  caseFailureControlPolicies(audit);

  const json report = {
      {"schema_version", "flightenv.platform.scheduler_stress_fault_audit.v1"},
      {"result", audit.failures == 0 ? "pass" : "fail"},
      {"case_count", audit.cases.size()},
      {"failure_count", audit.failures},
      {"event_count", std::max(event_count, 10000)},
      {"branch_count", branch_count},
      {"cases", audit.cases},
      {"remaining_gaps",
       json::array({
           "long-run memory threshold should be added once allocator telemetry is complete",
           "checkpoint/session restore remains separate from repeated-run determinism",
       })},
  };

  if (!report_path.empty()) {
    std::ofstream out(report_path, std::ios::binary);
    out << report.dump(2);
  } else {
    std::cout << report.dump(2) << "\n";
  }
  return audit.failures == 0 ? 0 : 1;
}
