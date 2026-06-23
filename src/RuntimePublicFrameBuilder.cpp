#include "FlightEnvPlatformRuntime/RuntimePublicFrameBuilder.hpp"
#include "FlightEnvPlatformRuntime/RuntimePortStoreView.hpp"
#include "FlightEnvPlatformRuntime/RuntimePublicFramePolicy.hpp"
#include "FlightEnvPlatformRuntime/RuntimeTimelineMaterializer.hpp"

#include <stdexcept>

namespace FlightEnvPlatformRuntime {
namespace {

std::string jsonString(const nlohmann::json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

void validateRequest(const RuntimePublicFrameRequest& request) {
  if (request.event == nullptr || request.tick == nullptr || request.nodes == nullptr ||
      request.time_scheduler == nullptr || request.outputs == nullptr ||
      request.public_tick_outputs == nullptr) {
    throw std::runtime_error("RuntimePublicFrameBuilder requires event, tick, nodes, scheduler, and output stores");
  }
  if (!request.outputs->is_object() || !request.public_tick_outputs->is_object()) {
    throw std::runtime_error("RuntimePublicFrameBuilder output stores must be JSON objects");
  }
}

nlohmann::json nodePlanInfo(const nlohmann::json* plan, const std::string& node_id) {
  if (plan != nullptr && plan->is_object() && plan->contains("nodes") && plan->at("nodes").is_array()) {
    for (const auto& item : plan->at("nodes")) {
      if (jsonString(item, "node_id") == node_id) {
        return item;
      }
    }
  }
  return nlohmann::json::object();
}

}  // namespace

RuntimePublicFrameResult RuntimePublicFrameBuilder::build(const RuntimePublicFrameRequest& request) {
  validateRequest(request);

  RuntimePublicFrameResult result;
  const RuntimeEvent& event = *request.event;
  const RuntimeLoopTick& tick = *request.tick;
  const int iteration = tick.iteration_index;
  const double public_output_time_s = tick.public_output_time_s;
  int dispatch_tick_index = request.dispatch_tick_index;

  result.loop_iteration_index = iteration;
  result.scheduler_events.push_back(
      RuntimePublicFramePolicy::makeLoopStartEvent(
          event,
          tick,
          request.external_observation,
          request.timestamp_utc));

  for (const auto& node : *request.nodes) {
    const std::string node_id = jsonString(node, "node_id");
    if (node_id.empty() || request.public_tick_outputs->contains(node_id)) {
      continue;
    }
    const nlohmann::json data_plane_info = nodePlanInfo(request.data_plane_plan, node_id);
    const nlohmann::json port_store_refs =
        request.port_store != nullptr
            ? RuntimePortStoreView::nodeOutputRefs(*request.port_store, node_id, data_plane_info)
            : nlohmann::json::object();
    const RuntimeNodeClockState* state = request.time_scheduler->stateFor(node_id);
    if (state == nullptr || !state->has_executed) {
      result.held_outputs.push_back(
          RuntimePublicFramePolicy::makeHeldOutput({
              &node,
              &data_plane_info,
              &port_store_refs,
              state,
              &event,
              &tick,
              request.timestamp_utc,
              "not_yet_due",
              dispatch_tick_index,
              false,
          }));
      continue;
    }

    (*request.outputs)[node_id] = state->last_execute_result;
    (*request.public_tick_outputs)[node_id] = state->last_execute_result;
    result.scheduler_events.push_back(
        RuntimePublicFramePolicy::makeHeldSchedulerEvent({
            &node,
            &data_plane_info,
            &port_store_refs,
            state,
            &event,
            &tick,
            request.timestamp_utc,
            "carried_forward_until_public_tick",
            dispatch_tick_index,
            true,
        }));
    ++dispatch_tick_index;
    result.held_outputs.push_back(
        RuntimePublicFramePolicy::makeHeldOutput({
            &node,
            &data_plane_info,
            &port_store_refs,
            state,
            &event,
            &tick,
            request.timestamp_utc,
            "carried_forward_until_public_tick",
            dispatch_tick_index - 1,
            true,
        }));
  }

  result.stop_status = RuntimePublicFramePolicy::stopStatus(*request.outputs, iteration, request.loop_policy);
  RuntimePublicFramePolicy::decorateStopStatus(
      result.stop_status,
      event,
      tick,
      request.effective_delta_t_s_by_node,
      result.held_outputs,
      request.public_tick_failed_nodes,
      request.base_dt_s,
      request.output_period_s,
      static_cast<int>(request.public_tick_outputs->size()));
  result.timeline_entries.push_back(
      RuntimeTimelineMaterializer::makeBranchStep(result.stop_status, "", iteration));
  result.stop_status["timeline_entry"] = result.timeline_entries.back();

  result.failed = request.public_tick_failed_nodes > 0;
  result.scheduler_events.push_back(
      RuntimePublicFramePolicy::makeLoopFinishEvent(
          event,
          tick,
          result.stop_status,
          static_cast<int>(result.held_outputs.size()),
          request.public_tick_failed_nodes,
          request.output_period_s,
          request.timestamp_utc));
  result.next_dispatch_tick_index = dispatch_tick_index;
  return result;
}

}  // namespace FlightEnvPlatformRuntime
