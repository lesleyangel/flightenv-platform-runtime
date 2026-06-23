#pragma once

/**
 * @file RuntimeReadyQueueExecutor.hpp
 * @brief Runtime ready queue facade for node admission and executor evidence.
 *
 * This component owns the platform-level admission checks that decide whether a
 * due node can enter execution. NativeWorkflowRunner should ask this facade
 * instead of embedding dependency, resource, parallel group, and deadline checks
 * inline in the event loop.
 */

#include "FlightEnvPlatform/Runtime/ReadyQueueScheduler.hpp"
#include "FlightEnvPlatform/Runtime/ThreadPoolExecutor.hpp"
#include "FlightEnvPlatform/Runtime/ThreadSafePortStore.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeEventQueue.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace FlightEnvPlatformRuntime {

struct RuntimeReadyAdmission {
  bool accepted = false;
  std::string status;
  std::string reason;
  std::string node_id;
  std::string operator_id;
  std::string runtime_event_id;
  std::string runtime_event_kind;
  std::vector<std::string> missing_dependencies;
  std::vector<std::string> missing_input_ports;
  std::vector<std::string> locked_resource_ids;
  std::vector<std::string> blocked_resource_ids;
  std::string parallel_group_id;
  std::string capacity_group;
  int capacity_group_limit = 0;
  std::string resource_lock_mode;
  std::string deadline_status;
  std::int64_t deadline_ns = 0;
  std::int64_t lateness_ns = 0;
  double deadline_s = 0.0;
  double lateness_s = 0.0;
  int scheduling_level = 0;
  bool can_run_parallel = true;
  nlohmann::json evidence = nlohmann::json::object();
};

class RuntimeReadyQueueExecutor {
 public:
  static RuntimeReadyQueueExecutor fromSchedulerPlan(const nlohmann::json& scheduler_plan);

  const flightenv::platform::ReadyQueueScheduler& scheduler() const;
  const flightenv::platform::ThreadPoolExecutorDescriptor& executor() const;

  nlohmann::json schedulerEvidence() const;
  nlohmann::json executorEvidence() const;
  nlohmann::json runtimeCoreEvidence() const;
  nlohmann::json readyQueueStateEvidence() const;

  RuntimeReadyAdmission admitNode(
      const nlohmann::json& node,
      const RuntimeEvent& event,
      const nlohmann::json& outputs,
      const flightenv::platform::ThreadSafePortStore& port_store,
      const std::string& branch_id = "",
      const std::string& timeline_id = "");
  void completeNode(const RuntimeReadyAdmission& admission);

 private:
  flightenv::platform::ReadyQueueScheduler scheduler_;
  flightenv::platform::ThreadPoolExecutorDescriptor executor_;
  std::map<std::string, nlohmann::json> plan_node_by_id_;
  std::map<std::string, std::string> active_resource_locks_;
  std::set<std::string> active_parallel_groups_;
  std::map<std::string, int> active_capacity_groups_;
  int active_execution_count_ = 0;
};

}  // namespace FlightEnvPlatformRuntime
