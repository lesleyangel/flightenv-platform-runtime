/**
 * @file RuntimeTimeScheduler.cpp
 * @brief 实现 runtime 时间调度门面。
 *
 * 大概：这是把事件队列、节点时钟、输入对齐和公开物化连接起来的协调层。
 * 具体：它应保持薄门面，把具体排序、样本选择、插值和物化细节委托给 time 子组件。
 * 被谁使用：被 NativeWorkflowRunner、PlatformRuntimeHost 和时间调度测试使用。
 * 使用谁：使用 time 子目录里的事件队列、样本缓冲、对齐和物化组件。
 * 拆分判断：当前可以总结；后续如果继续变厚，需要按 time 子职责继续下沉。
 */

#include "FlightEnvPlatformRuntime/RuntimeTimeScheduler.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace FlightEnvPlatformRuntime {
namespace {

using json = nlohmann::json;

std::string jsonString(const json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

double jsonDouble(const json& value, const std::string& key, double fallback = 0.0) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
    return fallback;
  }
  return value.at(key).get<double>();
}

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

json withLoopTime(json time_info, int iteration_index, double time_offset_s) {
  for (const std::string key : {"time_point", "output_time_point"}) {
    if (time_info.contains(key) && time_info.at(key).is_object()) {
      json& point = time_info[key];
      if (point.contains("run_time_s") && point.at("run_time_s").is_number()) {
        point["run_time_s"] = point.at("run_time_s").get<double>() + time_offset_s;
      }
      if (point.contains("source_time_s") && point.at("source_time_s").is_number()) {
        point["source_time_s"] = point.at("source_time_s").get<double>() + time_offset_s;
      }
      if (point.contains("tick_index") && point.at("tick_index").is_number_integer()) {
        point["tick_index"] = point.at("tick_index").get<int>() + iteration_index;
      }
    }
  }
  return time_info;
}

double timePointRunTimeS(const json& time_info, const std::string& key, double fallback) {
  if (!time_info.is_object() || !time_info.contains(key) || !time_info.at(key).is_object()) {
    return fallback;
  }
  return jsonDouble(time_info.at(key), "run_time_s", fallback);
}

double nodeOutputPeriodS(const json& time_info, const json& node, double base_dt_s) {
  const json policy = time_info.value("time_policy", node.value("time_policy", json::object()));
  const double sampled = jsonDouble(policy, "sample_period_s", 0.0);
  if (sampled > 0.0) {
    return sampled;
  }
  const double fixed = jsonDouble(policy, "fixed_dt_s", 0.0);
  if (fixed > 0.0) {
    return fixed;
  }
  const double delta = jsonDouble(time_info, "delta_t_s", 0.0);
  if (delta > 0.0) {
    return delta;
  }
  return base_dt_s > 0.0 ? base_dt_s : 0.0;
}

double publicOutputTimeS(const json& time_info, int iteration_index, double base_dt_s, double time_offset_s) {
  if (base_dt_s > 0.0) {
    return time_offset_s + base_dt_s;
  }
  return timePointRunTimeS(time_info, "output_time_point", static_cast<double>(iteration_index + 1));
}

json withDispatchTime(json time_info,
                      double effective_delta_t_s,
                      double output_period_s,
                      double public_output_time_s,
                      bool held,
                      double next_due_time_s,
                      const std::string& dispatch_reason) {
  time_info["effective_delta_t_s"] = effective_delta_t_s;
  time_info["output_period_s"] = output_period_s;
  time_info["public_output_time_s"] = public_output_time_s;
  time_info["held"] = held;
  time_info["dispatch_reason"] = dispatch_reason;
  if (std::isfinite(next_due_time_s)) {
    time_info["next_due_time_s"] = next_due_time_s;
  }
  time_info["public_tick"] = {
      {"output_time_s", public_output_time_s},
      {"effective_delta_t_s", effective_delta_t_s},
      {"output_period_s", output_period_s},
      {"held", held},
  };
  return time_info;
}

}  // namespace

RuntimeTimeScheduler::RuntimeTimeScheduler(nlohmann::json time_plan)
    : time_plan_(std::move(time_plan)) {}

nlohmann::json RuntimeTimeScheduler::planNodeInfo(const std::string& node_id) const {
  if (time_plan_.is_object() && time_plan_.contains("nodes") && time_plan_.at("nodes").is_array()) {
    for (const auto& item : time_plan_.at("nodes")) {
      if (jsonString(item, "node_id") == node_id) {
        return item;
      }
    }
  }
  return json::object();
}

nlohmann::json RuntimeTimeScheduler::baseTimeInfo(
    const nlohmann::json& node,
    int iteration_index,
    double time_offset_s) const {
  const std::string node_id = jsonString(node, "node_id");
  json time_info = planNodeInfo(node_id);
  if (time_info.empty()) {
    const json policy = node.value("time_policy", json::object());
    const double dt = jsonDouble(policy, "fixed_dt_s", jsonDouble(policy, "sample_period_s", 0.0));
    time_info = {
        {"node_id", node_id},
        {"operator_id", jsonString(node, "operator_id")},
        {"time_policy", policy},
        {"time_point", {{"run_time_s", 0.0}, {"tick_index", 0}, {"source_time_s", 0.0}, {"stamp_ns", 0}}},
        {"output_time_point",
         {{"run_time_s", dt},
          {"tick_index", 1},
          {"source_time_s", dt},
          {"stamp_ns", static_cast<long long>(dt * 1.0e9)}}},
        {"delta_t_s", dt},
        {"input_alignment", json::array()},
    };
  }
  return withLoopTime(time_info, iteration_index, time_offset_s);
}

RuntimeNodeDispatch RuntimeTimeScheduler::planDispatch(
    const nlohmann::json& node,
    int iteration_index,
    double base_dt_s,
    double time_offset_s) {
  constexpr double kEpsilon = 1.0e-9;

  const std::string node_id = jsonString(node, "node_id");
  const json base_time_info = baseTimeInfo(node, iteration_index, time_offset_s);
  RuntimeNodeClockState& state = state_by_node_[node_id];

  RuntimeNodeDispatch dispatch;
  dispatch.output_period_s = nodeOutputPeriodS(base_time_info, node, base_dt_s);
  dispatch.public_output_time_s = publicOutputTimeS(base_time_info, iteration_index, base_dt_s, time_offset_s);
  dispatch.held_from_iteration = state.last_execution_iteration;
  dispatch.last_execute_result = state.last_execute_result;
  dispatch.last_time_info = state.last_time_info;

  const double fallback_delta = base_dt_s > 0.0
                                    ? base_dt_s
                                    : jsonDouble(base_time_info, "delta_t_s", dispatch.output_period_s);
  if (!state.has_executed) {
    dispatch.execute = true;
    dispatch.reason = "initial_sample";
    dispatch.effective_delta_t_s = fallback_delta;
    dispatch.next_due_time_s = dispatch.public_output_time_s;
    dispatch.time_info = withDispatchTime(
        base_time_info,
        dispatch.effective_delta_t_s,
        dispatch.output_period_s,
        dispatch.public_output_time_s,
        false,
        dispatch.next_due_time_s,
        dispatch.reason);
    return dispatch;
  }

  const json policy = base_time_info.value("time_policy", node.value("time_policy", json::object()));
  const std::string kind = jsonString(policy, "kind");
  const bool coarse_sample =
      (kind == "sampled" || kind == "fixed_step") && dispatch.output_period_s > 0.0 &&
      base_dt_s > 0.0 && dispatch.output_period_s > base_dt_s + kEpsilon;
  dispatch.next_due_time_s = std::isfinite(state.next_due_public_time_s)
                                 ? state.next_due_public_time_s
                                 : state.last_execution_public_time_s + dispatch.output_period_s;
  if (!coarse_sample) {
    dispatch.execute = true;
    dispatch.reason = "every_public_tick";
  } else if (dispatch.public_output_time_s + kEpsilon >= dispatch.next_due_time_s) {
    dispatch.execute = true;
    dispatch.reason = "sample_period_due";
  } else {
    dispatch.execute = false;
    dispatch.reason = "sample_period_not_due";
  }

  if (std::isfinite(state.last_execution_public_time_s)) {
    dispatch.effective_delta_t_s =
        std::max(0.0, dispatch.public_output_time_s - state.last_execution_public_time_s);
  } else {
    dispatch.effective_delta_t_s = fallback_delta;
  }
  dispatch.time_info = withDispatchTime(
      base_time_info,
      dispatch.effective_delta_t_s,
      dispatch.output_period_s,
      dispatch.public_output_time_s,
      !dispatch.execute,
      dispatch.next_due_time_s,
      dispatch.reason);
  return dispatch;
}

RuntimeNodeDispatch RuntimeTimeScheduler::planEventDispatch(
    const nlohmann::json& node,
    const RuntimeEvent& event,
    double base_dt_s,
    double output_period_s) {
  const std::string node_id = jsonString(node, "node_id");
  const double public_period_s = runtimePublicPeriodS(base_dt_s, output_period_s);
  const double node_period_s = nodePeriodS(node, public_period_s);
  const json base_time_info = baseTimeInfo(node, event.iteration_index, 0.0);
  RuntimeNodeClockState& state = state_by_node_[node_id];

  RuntimeNodeDispatch dispatch;
  dispatch.execute = true;
  dispatch.reason = state.has_executed ? "node_due" : "initial_node_due";
  dispatch.output_period_s = node_period_s;
  dispatch.public_output_time_s = event.event_time_s;
  dispatch.next_due_time_s = event.event_time_s + node_period_s;
  dispatch.held_from_iteration = state.last_execution_iteration;
  dispatch.last_execute_result = state.last_execute_result;
  dispatch.last_time_info = state.last_time_info;
  dispatch.runtime_event_id = event.event_id;
  dispatch.runtime_event_kind = event.event_kind;
  dispatch.runtime_event_time_s = event.event_time_s;
  dispatch.effective_delta_t_s =
      state.has_executed && std::isfinite(state.last_execution_public_time_s)
          ? std::max(0.0, event.event_time_s - state.last_execution_public_time_s)
          : std::max(0.0, event.event_time_s);

  dispatch.time_info = withDispatchTime(
      base_time_info,
      dispatch.effective_delta_t_s,
      dispatch.output_period_s,
      dispatch.public_output_time_s,
      false,
      dispatch.next_due_time_s,
      dispatch.reason);
  const double input_time_s = std::max(0.0, event.event_time_s - dispatch.effective_delta_t_s);
  if (dispatch.time_info.contains("time_point") && dispatch.time_info.at("time_point").is_object()) {
    dispatch.time_info["time_point"]["run_time_s"] = input_time_s;
    dispatch.time_info["time_point"]["source_time_s"] = input_time_s;
    dispatch.time_info["time_point"]["stamp_ns"] = static_cast<long long>(input_time_s * 1.0e9);
  }
  if (dispatch.time_info.contains("output_time_point") &&
      dispatch.time_info.at("output_time_point").is_object()) {
    dispatch.time_info["output_time_point"]["run_time_s"] = event.event_time_s;
    dispatch.time_info["output_time_point"]["source_time_s"] = event.event_time_s;
    dispatch.time_info["output_time_point"]["stamp_ns"] = static_cast<long long>(event.event_time_s * 1.0e9);
  }
  dispatch.time_info["runtime_event"] = {
      {"event_id", event.event_id},
      {"event_kind", event.event_kind},
      {"event_time_s", event.event_time_s},
      {"target_id", event.target_id},
      {"cause_event_id", event.cause_event_id},
      {"payload", event.payload},
  };
  return dispatch;
}

void RuntimeTimeScheduler::seedWorkflowEvents(
    RuntimeEventQueue& events,
    const std::vector<nlohmann::json>& nodes,
    int max_iterations,
    double base_dt_s,
    double output_period_s) const {
  const int bounded_iterations = std::max(1, max_iterations);
  const double public_period_s = runtimePublicPeriodS(base_dt_s, output_period_s);
  const double horizon_s = static_cast<double>(bounded_iterations) * public_period_s;
  constexpr double kEpsilon = 1.0e-9;

  events.pushFixedPublicTicks(bounded_iterations, base_dt_s, public_period_s);
  for (std::size_t node_index = 0; node_index < nodes.size(); ++node_index) {
    const json& node = nodes[node_index];
    const std::string node_id = jsonString(node, "node_id");
    if (node_id.empty()) {
      continue;
    }
    const double period_s = nodePeriodS(node, public_period_s);
    if (!(period_s > kEpsilon) || !std::isfinite(period_s)) {
      continue;
    }

    const double first_due_time_s =
        period_s > public_period_s + kEpsilon ? public_period_s : period_s;
    for (int due_index = 0; ; ++due_index) {
      const double due_time_s = first_due_time_s + period_s * static_cast<double>(due_index);
      if (due_time_s > horizon_s + kEpsilon) {
        break;
      }
      const int public_iteration = publicIterationForTime(due_time_s, public_period_s);
      events.push(RuntimeEventQueue::nodeDueEvent(
          node_id,
          due_time_s,
          public_iteration,
          10 + static_cast<int>(node_index),
          {
              {"node_id", node_id},
              {"topological_index", static_cast<int>(node_index)},
              {"sample_period_s", period_s},
              {"public_period_s", public_period_s},
              {"horizon_s", horizon_s},
              {"due_index", due_index},
          }));
    }
  }
}

void RuntimeTimeScheduler::markExecuted(
    const std::string& node_id,
    int iteration_index,
    const RuntimeNodeDispatch& dispatch,
    const nlohmann::json& execute_result) {
  RuntimeNodeClockState& state = state_by_node_[node_id];
  state.has_executed = true;
  state.last_execution_iteration = iteration_index;
  state.last_execution_public_time_s = dispatch.public_output_time_s;
  state.next_due_public_time_s =
      dispatch.output_period_s > 0.0
          ? dispatch.public_output_time_s + dispatch.output_period_s
          : dispatch.public_output_time_s + 1.0;
  state.last_execute_result = execute_result;
  state.last_time_info = dispatch.time_info;
}

const RuntimeNodeClockState* RuntimeTimeScheduler::stateFor(const std::string& node_id) const {
  const auto found = state_by_node_.find(node_id);
  return found == state_by_node_.end() ? nullptr : &found->second;
}

double RuntimeTimeScheduler::nodePeriodS(const nlohmann::json& node, double fallback_period_s) const {
  const json time_info = planNodeInfo(jsonString(node, "node_id"));
  const json policy = time_info.value("time_policy", node.value("time_policy", json::object()));
  const double sampled = jsonDouble(policy, "sample_period_s", 0.0);
  if (sampled > 0.0) {
    return sampled;
  }
  const double fixed = jsonDouble(policy, "fixed_dt_s", 0.0);
  if (fixed > 0.0) {
    return fixed;
  }
  const double delta = jsonDouble(time_info, "delta_t_s", 0.0);
  if (delta > 0.0) {
    return delta;
  }
  return fallback_period_s > 0.0 ? fallback_period_s : 1.0;
}

int RuntimeTimeScheduler::publicIterationForTime(double event_time_s, double public_period_s) const {
  if (!(public_period_s > 0.0) || !std::isfinite(event_time_s)) {
    return 0;
  }
  constexpr double kEpsilon = 1.0e-9;
  const double scaled = std::ceil(std::max(0.0, event_time_s - kEpsilon) / public_period_s);
  return std::max(0, static_cast<int>(scaled) - 1);
}

double RuntimeTimeScheduler::loopTimeOffsetS(int iteration_index, double base_dt_s) {
  return runtimeLoopTimeOffsetS(iteration_index, base_dt_s);
}

double RuntimeTimeScheduler::loopPublicOutputTimeS(int iteration_index, double base_dt_s) {
  return runtimeLoopPublicOutputTimeS(iteration_index, base_dt_s);
}

}  // namespace FlightEnvPlatformRuntime
