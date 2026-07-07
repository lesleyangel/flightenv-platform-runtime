#pragma once

#include "FlightEnvPlatformRuntime/RuntimePortBindingResolver.hpp"
#include "FlightEnvPlatformRuntime/RuntimeRateTransitionExecutor.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeInputAlignment.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimePortSampleBuffer.hpp"

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace flightenv::platform {
class ThreadSafePortStore;
}

namespace FlightEnvPlatformRuntime {

struct NativeWorkflowNodeInputPreparationRequest {
  std::string run_id;
  std::string object_id;
  std::string branch_id;
  std::string timeline_id;
  std::string node_id;
  nlohmann::json node = nlohmann::json::object();
  nlohmann::json base_upstream = nlohmann::json::object();
  nlohmann::json external_seed = nlohmann::json::object();
  nlohmann::json current_outputs = nlohmann::json::object();
  nlohmann::json data_plane_info = nlohmann::json::object();
  nlohmann::json time_info = nlohmann::json::object();
  std::int64_t event_time_ns = 0;
  int iteration_index = 0;
  flightenv::platform::ThreadSafePortStore* port_store = nullptr;
  RuntimePortSampleBuffer* sample_buffer = nullptr;
  bool allow_implicit_contract_port_binding = false;
  std::function<void(const std::string&)> trace;
};

struct NativeWorkflowNodeInputPreparationResult {
  RuntimePortBindingResolveResult port_binding;
  RuntimeRateTransitionExecutionResult transition_execution;
  RuntimeInputAlignmentResult input_alignment;
  nlohmann::json upstream = nlohmann::json::object();
};

NativeWorkflowNodeInputPreparationResult prepareNativeWorkflowNodeInputs(
    const NativeWorkflowNodeInputPreparationRequest& request);

}  // namespace FlightEnvPlatformRuntime
