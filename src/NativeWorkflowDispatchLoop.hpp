#pragma once

#include "FlightEnvPlatformRuntime/RuntimeReadyQueueExecutor.hpp"
#include "FlightEnvPlatformRuntime/RuntimeTimeScheduler.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeInputAlignment.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeEventQueue.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

void scheduleNativeWorkflowNodeDueRetryOrDrop(
    RuntimeEventQueue& event_queue,
    nlohmann::json& scheduler_events,
    const RuntimeEvent& blocked_event,
    const std::string& blocked_node_id,
    const std::string& blocked_reason);

nlohmann::json nativeWorkflowInputBindingSchedulerEvent(
    const RuntimeEvent& event,
    const std::string& node_id,
    const nlohmann::json& input_binding_evidence);

nlohmann::json nativeWorkflowInputAlignmentBlockedEvent(
    const RuntimeEvent& event,
    const std::string& node_id,
    int dispatch_tick_index,
    const nlohmann::json& input_binding_evidence,
    const RuntimeInputAlignmentResult& input_alignment);

nlohmann::json nativeWorkflowReadyQueueAdmissionEvent(
    const RuntimeEvent& event,
    const std::string& node_id,
    int dispatch_tick_index,
    const nlohmann::json& input_binding_evidence,
    const RuntimeReadyAdmission& admission);

nlohmann::json nativeWorkflowNodeStartEvent(
    const RuntimeEvent& event,
    const std::string& node_id,
    int dispatch_tick_index,
    const RuntimeNodeDispatch& cadence,
    const nlohmann::json& input_binding_evidence,
    const RuntimeInputAlignmentResult& input_alignment,
    const RuntimeReadyAdmission& admission,
    int scheduling_level,
    const std::string& parallel_group_id,
    std::int64_t execution_started_steady_ns);

nlohmann::json nativeWorkflowNodeFinishOkEvent(
    const RuntimeEvent& event,
    const std::string& node_id,
    int dispatch_tick_index,
    const RuntimeNodeDispatch& cadence,
    int duration_ms,
    std::int64_t execution_started_steady_ns,
    std::int64_t execution_finished_steady_ns);

nlohmann::json nativeWorkflowNodeFinishFailedEvent(
    const RuntimeEvent& event,
    const std::string& node_id,
    const std::string& reason,
    std::int64_t execution_started_steady_ns,
    std::int64_t execution_finished_steady_ns);

}  // namespace FlightEnvPlatformRuntime
