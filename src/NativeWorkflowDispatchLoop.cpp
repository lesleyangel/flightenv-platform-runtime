#include "NativeWorkflowDispatchLoop.hpp"

#include "FlightEnvPlatformRuntime/RuntimeClock.hpp"
#include "FlightEnvPlatformRuntime/RuntimeEventLoop.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

using json = nlohmann::json;

namespace FlightEnvPlatformRuntime {
namespace {

constexpr int kNodeDueRetryPriority = 60000;
constexpr int kNodeDueMaxRetries = 1;

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

int loopIterationIndex(const RuntimeEvent& event) {
  return std::max(0, event.iteration_index);
}

json eventTimeEvidence(const RuntimeEvent& event) {
  return {
      {"runtime_event_id", event.event_id},
      {"runtime_event_kind", event.event_kind},
      {"runtime_event_time_s", event.event_time_s},
      {"runtime_event_time_ns", event.event_time.nanoseconds},
      {"loop_iteration_index", loopIterationIndex(event)},
  };
}

json executionElapsedMs(std::int64_t started_ns, std::int64_t finished_ns) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::nanoseconds(finished_ns - started_ns))
      .count();
}

}  // namespace

void scheduleNativeWorkflowNodeDueRetryOrDrop(
    RuntimeEventQueue& event_queue,
    json& scheduler_events,
    const RuntimeEvent& blocked_event,
    const std::string& blocked_node_id,
    const std::string& blocked_reason) {
  const int retry_count = jsonInt(blocked_event.payload, "retry_count", 0);
  if (retry_count < kNodeDueMaxRetries) {
    RuntimeEvent retry_event = blocked_event;
    retry_event.event_id.clear();
    retry_event.priority = kNodeDueRetryPriority;
    retry_event.payload["retry_count"] = retry_count + 1;
    retry_event.payload["retry_of_event_id"] = blocked_event.event_id;
    retry_event.payload["retry_reason"] = blocked_reason;
    event_queue.push(std::move(retry_event));
    scheduler_events.push_back({
        {"timestamp_utc", RuntimeClock::nowUtcIso()},
        {"node_id", blocked_node_id},
        {"event", "node_due_retry_scheduled"},
        {"status", "deferred"},
        {"reason", blocked_reason},
        {"retry_count", retry_count + 1},
        {"runtime_event_id", blocked_event.event_id},
        {"runtime_event_time_s", blocked_event.event_time_s},
        {"runtime_event_time_ns", blocked_event.event_time.nanoseconds},
        {"loop_iteration_index", loopIterationIndex(blocked_event)},
    });
    return;
  }
  scheduler_events.push_back({
      {"timestamp_utc", RuntimeClock::nowUtcIso()},
      {"node_id", blocked_node_id},
      {"event", "node_due_dropped"},
      {"status", "dropped"},
      {"reason", blocked_reason},
      {"retry_count", retry_count},
      {"runtime_event_id", blocked_event.event_id},
      {"runtime_event_time_s", blocked_event.event_time_s},
      {"runtime_event_time_ns", blocked_event.event_time.nanoseconds},
      {"loop_iteration_index", loopIterationIndex(blocked_event)},
  });
}

json nativeWorkflowInputBindingSchedulerEvent(
    const RuntimeEvent& event,
    const std::string& node_id,
    const json& input_binding_evidence) {
  json scheduler_event = RuntimeEventLoop::schedulerEvent(
      event,
      "input_binding",
      input_binding_evidence);
  scheduler_event["node_id"] = node_id;
  scheduler_event["input_binding"] = input_binding_evidence;
  return scheduler_event;
}

json nativeWorkflowInputAlignmentBlockedEvent(
    const RuntimeEvent& event,
    const std::string& node_id,
    int dispatch_tick_index,
    const json& input_binding_evidence,
    const RuntimeInputAlignmentResult& input_alignment) {
  json result = {
      {"timestamp_utc", RuntimeClock::nowUtcIso()},
      {"node_id", node_id},
      {"event", "input_alignment_blocked"},
      {"status", "blocked"},
      {"reason", "required_rate_transition_input_unavailable"},
      {"dispatch_tick_index", dispatch_tick_index},
      {"input_binding", input_binding_evidence},
      {"input_alignment", input_alignment.evidence()},
      {"unavailable_required_inputs", input_alignment.unavailableRequiredEvidence()},
  };
  result.update(eventTimeEvidence(event));
  return result;
}

json nativeWorkflowReadyQueueAdmissionEvent(
    const RuntimeEvent& event,
    const std::string& node_id,
    int dispatch_tick_index,
    const json& input_binding_evidence,
    const RuntimeReadyAdmission& admission) {
  json result = {
      {"timestamp_utc", RuntimeClock::nowUtcIso()},
      {"node_id", node_id},
      {"event", "ready_queue_admission"},
      {"status", admission.status},
      {"accepted", admission.accepted},
      {"reason", admission.reason},
      {"dispatch_tick_index", dispatch_tick_index},
      {"input_binding", input_binding_evidence},
      {"dependency_check", admission.evidence.value("dependency_check", json::object())},
      {"port_check", admission.evidence.value("port_check", json::object())},
      {"resource_check", admission.evidence.value("resource_check", json::object())},
      {"parallel_check", admission.evidence.value("parallel_check", json::object())},
      {"deadline_check", admission.evidence.value("deadline_check", json::object())},
      {"ready_queue", admission.evidence},
  };
  result.update(eventTimeEvidence(event));
  return result;
}

json nativeWorkflowNodeStartEvent(
    const RuntimeEvent& event,
    const std::string& node_id,
    int dispatch_tick_index,
    const RuntimeNodeDispatch& cadence,
    const json& input_binding_evidence,
    const RuntimeInputAlignmentResult& input_alignment,
    const RuntimeReadyAdmission& admission,
    int scheduling_level,
    const std::string& parallel_group_id,
    std::int64_t execution_started_steady_ns) {
  json result = {
      {"timestamp_utc", RuntimeClock::nowUtcIso()},
      {"node_id", node_id},
      {"event", "start"},
      {"status", "running"},
      {"dispatch_tick_index", dispatch_tick_index},
      {"dispatch_time_s", cadence.public_output_time_s},
      {"dispatch_time_ns", cadence.public_output_time.nanoseconds},
      {"execution_started_steady_ns", execution_started_steady_ns},
      {"output_period_s", cadence.output_period_s},
      {"output_period_ns", cadence.output_period.nanoseconds},
      {"effective_delta_t_s", cadence.effective_delta_t_s},
      {"effective_delta_t_ns", cadence.effective_delta_t.nanoseconds},
      {"next_due_time_s", cadence.next_due_time_s},
      {"next_due_time_ns", cadence.next_due_time.nanoseconds},
      {"input_binding", input_binding_evidence},
      {"input_alignment", input_alignment.evidence()},
      {"ready_queue", admission.evidence},
      {"scheduling_level", scheduling_level},
      {"parallel_group_id", parallel_group_id},
  };
  result.update(eventTimeEvidence(event));
  return result;
}

json nativeWorkflowNodeFinishOkEvent(
    const RuntimeEvent& event,
    const std::string& node_id,
    int dispatch_tick_index,
    const RuntimeNodeDispatch& cadence,
    int duration_ms,
    std::int64_t execution_started_steady_ns,
    std::int64_t execution_finished_steady_ns) {
  json result = {
      {"timestamp_utc", RuntimeClock::nowUtcIso()},
      {"node_id", node_id},
      {"event", "finish"},
      {"status", "ok"},
      {"duration_ms", duration_ms},
      {"dispatch_tick_index", dispatch_tick_index},
      {"execution_started_steady_ns", execution_started_steady_ns},
      {"execution_finished_steady_ns", execution_finished_steady_ns},
      {"execution_elapsed_ms", executionElapsedMs(execution_started_steady_ns, execution_finished_steady_ns)},
      {"output_period_s", cadence.output_period_s},
      {"output_period_ns", cadence.output_period.nanoseconds},
      {"effective_delta_t_s", cadence.effective_delta_t_s},
      {"effective_delta_t_ns", cadence.effective_delta_t.nanoseconds},
  };
  result.update(eventTimeEvidence(event));
  return result;
}

json nativeWorkflowNodeFinishFailedEvent(
    const RuntimeEvent& event,
    const std::string& node_id,
    const std::string& reason,
    std::int64_t execution_started_steady_ns,
    std::int64_t execution_finished_steady_ns) {
  json result = {
      {"timestamp_utc", RuntimeClock::nowUtcIso()},
      {"node_id", node_id},
      {"event", "finish"},
      {"status", "failed"},
      {"reason", reason},
      {"execution_started_steady_ns", execution_started_steady_ns},
      {"execution_finished_steady_ns", execution_finished_steady_ns},
      {"execution_elapsed_ms", executionElapsedMs(execution_started_steady_ns, execution_finished_steady_ns)},
  };
  result.update(eventTimeEvidence(event));
  return result;
}

}  // namespace FlightEnvPlatformRuntime
