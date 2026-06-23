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
#include <cstdint>
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
                      RuntimeDuration effective_delta_t,
                      RuntimeDuration output_period,
                      RuntimeTimePoint public_output_time,
                      double effective_delta_t_s,
                      double output_period_s,
                      double public_output_time_s,
                      bool held,
                      double next_due_time_s,
                      const std::string& dispatch_reason) {
  time_info["effective_delta_t_s"] = effective_delta_t_s;
  time_info["effective_delta_t_ns"] = effective_delta_t.nanoseconds;
  time_info["output_period_s"] = output_period_s;
  time_info["output_period_ns"] = output_period.nanoseconds;
  time_info["public_output_time_s"] = public_output_time_s;
  time_info["public_output_time_ns"] = public_output_time.nanoseconds;
  time_info["held"] = held;
  time_info["dispatch_reason"] = dispatch_reason;
  if (std::isfinite(next_due_time_s)) {
    time_info["next_due_time_s"] = next_due_time_s;
  }
  time_info["public_tick"] = {
      {"output_time_s", public_output_time_s},
      {"output_time_ns", public_output_time.nanoseconds},
      {"effective_delta_t_s", effective_delta_t_s},
      {"effective_delta_t_ns", effective_delta_t.nanoseconds},
      {"output_period_s", output_period_s},
      {"output_period_ns", output_period.nanoseconds},
      {"held", held},
  };
  return time_info;
}

int publicIterationForTimePoint(RuntimeTimePoint event_time, RuntimeDuration public_period) {
  if (public_period.nanoseconds <= 0) {
    return 0;
  }
  const std::int64_t clamped_time = std::max<std::int64_t>(0, event_time.nanoseconds);
  const std::int64_t scaled =
      (clamped_time + public_period.nanoseconds - 1) / public_period.nanoseconds;
  return std::max(0, static_cast<int>(scaled) - 1);
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
  const std::string node_id = jsonString(node, "node_id");
  const json base_time_info = baseTimeInfo(node, iteration_index, time_offset_s);
  RuntimeNodeClockState& state = state_by_node_[node_id];

  RuntimeNodeDispatch dispatch;
  dispatch.output_period_s = nodeOutputPeriodS(base_time_info, node, base_dt_s);
  dispatch.output_period = RuntimeDuration::fromSeconds(dispatch.output_period_s);
  dispatch.public_output_time_s = publicOutputTimeS(base_time_info, iteration_index, base_dt_s, time_offset_s);
  dispatch.public_output_time = RuntimeTimePoint::fromSeconds(dispatch.public_output_time_s);
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
    dispatch.effective_delta_t = RuntimeDuration::fromSeconds(dispatch.effective_delta_t_s);
    dispatch.next_due_time_s = dispatch.public_output_time_s;
    dispatch.next_due_time = dispatch.public_output_time;
    dispatch.time_info = withDispatchTime(
        base_time_info,
        dispatch.effective_delta_t,
        dispatch.output_period,
        dispatch.public_output_time,
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
  const RuntimeDuration base_dt = RuntimeDuration::fromSeconds(base_dt_s);
  const bool coarse_sample =
      (kind == "sampled" || kind == "fixed_step") &&
      dispatch.output_period.nanoseconds > 0 &&
      base_dt.nanoseconds > 0 &&
      dispatch.output_period.nanoseconds > base_dt.nanoseconds;
  dispatch.next_due_time =
      state.next_due_public_time.nanoseconds > 0
          ? state.next_due_public_time
          : state.last_execution_public_time + dispatch.output_period;
  dispatch.next_due_time_s = dispatch.next_due_time.seconds();
  if (!coarse_sample) {
    dispatch.execute = true;
    dispatch.reason = "every_public_tick";
  } else if (dispatch.public_output_time.nanoseconds >= dispatch.next_due_time.nanoseconds) {
    dispatch.execute = true;
    dispatch.reason = "sample_period_due";
  } else {
    dispatch.execute = false;
    dispatch.reason = "sample_period_not_due";
  }

  if (std::isfinite(state.last_execution_public_time_s)) {
    dispatch.effective_delta_t = dispatch.public_output_time - state.last_execution_public_time;
    if (dispatch.effective_delta_t.nanoseconds < 0) {
      dispatch.effective_delta_t = RuntimeDuration::fromNanoseconds(0);
    }
    dispatch.effective_delta_t_s = dispatch.effective_delta_t.seconds();
  } else {
    dispatch.effective_delta_t_s = fallback_delta;
    dispatch.effective_delta_t = RuntimeDuration::fromSeconds(dispatch.effective_delta_t_s);
  }
  dispatch.time_info = withDispatchTime(
      base_time_info,
      dispatch.effective_delta_t,
      dispatch.output_period,
      dispatch.public_output_time,
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
  dispatch.output_period = RuntimeDuration::fromSeconds(node_period_s);
  dispatch.public_output_time = event.event_time;
  dispatch.next_due_time = event.event_time + dispatch.output_period;
  dispatch.output_period_s = node_period_s;
  dispatch.public_output_time_s = dispatch.public_output_time.seconds();
  dispatch.next_due_time_s = dispatch.next_due_time.seconds();
  dispatch.held_from_iteration = state.last_execution_iteration;
  dispatch.last_execute_result = state.last_execute_result;
  dispatch.last_time_info = state.last_time_info;
  dispatch.runtime_event_id = event.event_id;
  dispatch.runtime_event_kind = event.event_kind;
  dispatch.runtime_event_time = event.event_time;
  dispatch.runtime_event_time_s = event.event_time_s;
  dispatch.effective_delta_t =
      state.has_executed
          ? event.event_time - state.last_execution_public_time
          : RuntimeDuration::fromNanoseconds(std::max<std::int64_t>(0, event.event_time.nanoseconds));
  if (dispatch.effective_delta_t.nanoseconds < 0) {
    dispatch.effective_delta_t = RuntimeDuration::fromNanoseconds(0);
  }
  dispatch.effective_delta_t_s = dispatch.effective_delta_t.seconds();

  dispatch.time_info = withDispatchTime(
      base_time_info,
      dispatch.effective_delta_t,
      dispatch.output_period,
      dispatch.public_output_time,
      dispatch.effective_delta_t_s,
      dispatch.output_period_s,
      dispatch.public_output_time_s,
      false,
      dispatch.next_due_time_s,
      dispatch.reason);
  const RuntimeTimePoint input_time = RuntimeTimePoint::fromNanoseconds(
      std::max<std::int64_t>(0, event.event_time.nanoseconds - dispatch.effective_delta_t.nanoseconds));
  const double input_time_s = input_time.seconds();
  if (dispatch.time_info.contains("time_point") && dispatch.time_info.at("time_point").is_object()) {
    dispatch.time_info["time_point"]["run_time_s"] = input_time_s;
    dispatch.time_info["time_point"]["source_time_s"] = input_time_s;
    dispatch.time_info["time_point"]["stamp_ns"] = input_time.nanoseconds;
  }
  if (dispatch.time_info.contains("output_time_point") &&
      dispatch.time_info.at("output_time_point").is_object()) {
    dispatch.time_info["output_time_point"]["run_time_s"] = dispatch.public_output_time_s;
    dispatch.time_info["output_time_point"]["source_time_s"] = dispatch.public_output_time_s;
    dispatch.time_info["output_time_point"]["stamp_ns"] = dispatch.public_output_time.nanoseconds;
  }
  dispatch.time_info["runtime_event"] = {
      {"event_id", event.event_id},
      {"event_kind", event.event_kind},
      {"event_time_s", event.event_time_s},
      {"event_time_ns", event.event_time.nanoseconds},
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
  const RuntimeDuration public_period = RuntimeDuration::fromSeconds(public_period_s);
  const RuntimeTimePoint horizon =
      RuntimeTimePoint::fromNanoseconds((public_period * bounded_iterations).nanoseconds);
  const double horizon_s = horizon.seconds();
  constexpr double kEpsilon = 1.0e-9;

  events.pushFixedPublicTicks(bounded_iterations, base_dt_s, public_period_s);
  for (int iteration = 0; iteration < bounded_iterations; ++iteration) {
    const RuntimeTimePoint public_time =
        RuntimeTimePoint::fromNanoseconds((public_period * iteration).nanoseconds);
    const double public_time_s = public_time.seconds();
    events.push(RuntimeEventQueue::checkpointDueEvent(
        "workflow_public_checkpoint",
        public_time_s,
        iteration,
        {
            {"checkpoint_scope", "workflow_public_tick"},
            {"public_iteration_index", iteration},
            {"public_period_s", public_period_s},
            {"public_period_ns", public_period.nanoseconds},
            {"event_time_ns", public_time.nanoseconds},
        }));
    events.push(RuntimeEventQueue::stopCheckDueEvent(
        public_time_s,
        iteration,
        {
            {"stop_scope", "workflow_public_tick"},
            {"public_iteration_index", iteration},
            {"public_period_s", public_period_s},
            {"public_period_ns", public_period.nanoseconds},
            {"event_time_ns", public_time.nanoseconds},
        }));
  }
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
    const RuntimeDuration period = RuntimeDuration::fromSeconds(period_s);
    if (period.nanoseconds <= 0) {
      continue;
    }

    const RuntimeTimePoint first_due_time =
        period.nanoseconds > public_period.nanoseconds
            ? RuntimeTimePoint::fromNanoseconds(public_period.nanoseconds)
            : RuntimeTimePoint::fromNanoseconds(period.nanoseconds);
    for (int due_index = 0; ; ++due_index) {
      const RuntimeTimePoint due_time = first_due_time + (period * due_index);
      if (due_time.nanoseconds > horizon.nanoseconds) {
        break;
      }
      const double due_time_s = due_time.seconds();
      const int public_iteration = publicIterationForTimePoint(due_time, public_period);
      events.push(RuntimeEventQueue::nodeDueEvent(
          node_id,
          due_time_s,
          public_iteration,
          10 + static_cast<int>(node_index),
          {
              {"node_id", node_id},
              {"topological_index", static_cast<int>(node_index)},
              {"sample_period_s", period_s},
              {"sample_period_ns", period.nanoseconds},
              {"scheduled_event_time_s", due_time_s},
              {"scheduled_event_time_ns", due_time.nanoseconds},
              {"public_period_s", public_period_s},
              {"public_period_ns", public_period.nanoseconds},
              {"horizon_s", horizon_s},
              {"horizon_ns", horizon.nanoseconds},
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
  state.last_execution_public_time = dispatch.public_output_time;
  state.next_due_public_time =
      dispatch.output_period.nanoseconds > 0
          ? dispatch.public_output_time + dispatch.output_period
          : dispatch.public_output_time + RuntimeDuration::fromSeconds(1.0);
  state.last_execution_public_time_s = dispatch.public_output_time_s;
  state.next_due_public_time_s = state.next_due_public_time.seconds();
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
  return publicIterationForTimePoint(
      RuntimeTimePoint::fromSeconds(event_time_s),
      RuntimeDuration::fromSeconds(public_period_s));
}

double RuntimeTimeScheduler::loopTimeOffsetS(int iteration_index, double base_dt_s) {
  return runtimeLoopTimeOffsetS(iteration_index, base_dt_s);
}

double RuntimeTimeScheduler::loopPublicOutputTimeS(int iteration_index, double base_dt_s) {
  return runtimeLoopPublicOutputTimeS(iteration_index, base_dt_s);
}

}  // namespace FlightEnvPlatformRuntime
