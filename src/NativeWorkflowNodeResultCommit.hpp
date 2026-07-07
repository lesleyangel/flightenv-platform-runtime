#pragma once

#include "FlightEnvPlatform/Runtime/RuntimePacket.hpp"
#include "FlightEnvPlatformRuntime/RuntimeNodeEvidenceBuilder.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace flightenv::platform {
class ThreadSafePortStore;
}

namespace FlightEnvPlatformRuntime {

nlohmann::json nativeWorkflowRuntimePacketToJson(
    const flightenv::platform::RuntimePacket& packet);

struct NativeWorkflowNodeResultCommitRequest {
  std::string run_id;
  std::string object_id;
  std::string branch_id;
  std::string timeline_id;
  std::string node_id;
  std::string operator_id;
  std::string adapter_id;
  std::string execution_kind;
  std::string adapter_protocol;
  std::string execution_tag;
  int iteration_index = 0;
  nlohmann::json time_info = nlohmann::json::object();
  nlohmann::json data_plane_info = nlohmann::json::object();
  nlohmann::json upstream = nlohmann::json::object();
  nlohmann::json execute_result = nlohmann::json::object();
  nlohmann::json adapter_snapshot = nlohmann::json::object();
  nlohmann::json uncertainty_info = nlohmann::json::object();
  nlohmann::json state_store_info = nlohmann::json::object();
  nlohmann::json input_summary = nlohmann::json::object();
  nlohmann::json output_summary = nlohmann::json::object();
  flightenv::platform::ThreadSafePortStore* port_store = nullptr;
  std::function<void(const std::string&)> trace;
};

struct NativeWorkflowNodeResultCommitResult {
  nlohmann::json data_plane_entries = nlohmann::json::array();
  nlohmann::json runtime_packet = nlohmann::json::object();
  nlohmann::json runtime_port_packets = nlohmann::json::array();
  nlohmann::json runtime_port_packet_by_port = nlohmann::json::object();
  RuntimeNodeEvidenceResult node_evidence;
};

NativeWorkflowNodeResultCommitResult commitNativeWorkflowNodeResult(
    const NativeWorkflowNodeResultCommitRequest& request);

}  // namespace FlightEnvPlatformRuntime
