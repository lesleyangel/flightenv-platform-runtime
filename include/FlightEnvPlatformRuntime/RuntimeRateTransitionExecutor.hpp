#pragma once

/**
 * @file RuntimeRateTransitionExecutor.hpp
 * @brief Executes compiled rate transitions as platform-owned synthetic nodes.
 *
 * The executor is intentionally platform-neutral: it reads declared transition
 * policies, queries the runtime sample buffer, writes a synthetic output packet,
 * and records evidence. Object-specific meaning remains in object/operator
 * packages.
 */

#include "FlightEnvPlatformRuntime/time/RuntimePortSampleBuffer.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace flightenv::platform {
class ThreadSafePortStore;
}

namespace FlightEnvPlatformRuntime {

struct RuntimeRateTransitionExecutionRequest {
  std::string run_id;
  std::string object_id;
  std::string branch_id;
  std::string timeline_id;
  std::string target_node_id;
  nlohmann::json target_node = nlohmann::json::object();
  nlohmann::json time_info = nlohmann::json::object();
  int iteration_index = 0;
  RuntimePortSampleBuffer* sample_buffer = nullptr;
  flightenv::platform::ThreadSafePortStore* port_store = nullptr;
};

struct RuntimeRateTransitionExecutionResult {
  int transition_count = 0;
  int executed_count = 0;
  int blocked_count = 0;
  int packet_count = 0;
  int sample_count = 0;
  nlohmann::json events = nlohmann::json::array();
  nlohmann::json packets = nlohmann::json::array();
  nlohmann::json outputs_by_transition_node = nlohmann::json::object();
  nlohmann::json unavailable_transitions = nlohmann::json::array();

  bool hasEvidence() const;
};

class RuntimeRateTransitionExecutor {
 public:
  static RuntimeRateTransitionExecutionResult executeForTarget(
      const RuntimeRateTransitionExecutionRequest& request);
};

}  // namespace FlightEnvPlatformRuntime
