#include "FlightEnvPlatformRuntime/RuntimePublicFramePolicy.hpp"

#include <cmath>
#include <limits>

namespace FlightEnvPlatformRuntime {
namespace {

std::string jsonString(const nlohmann::json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

double jsonDouble(const nlohmann::json& value, const std::string& key, double fallback = 0.0) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
    return fallback;
  }
  return value.at(key).get<double>();
}

int jsonInt(const nlohmann::json& value, const std::string& key, int fallback = 0) {
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

double findNumberRecursive(const nlohmann::json& value, const std::vector<std::string>& keys, int depth = 0) {
  if (depth > 24) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (value.is_object()) {
    for (const auto& key : keys) {
      auto it = value.find(key);
      if (it != value.end() && it->is_number()) {
        return it->get<double>();
      }
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
      const double found = findNumberRecursive(it.value(), keys, depth + 1);
      if (std::isfinite(found)) {
        return found;
      }
    }
  } else if (value.is_array()) {
    for (const auto& item : value) {
      const double found = findNumberRecursive(item, keys, depth + 1);
      if (std::isfinite(found)) {
        return found;
      }
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

bool compareMetric(double value, const std::string& comparator, double threshold) {
  if (!std::isfinite(value)) {
    return false;
  }
  if (comparator == "<" || comparator == "lt") return value < threshold;
  if (comparator == "<=" || comparator == "le") return value <= threshold;
  if (comparator == ">" || comparator == "gt") return value > threshold;
  if (comparator == ">=" || comparator == "ge") return value >= threshold;
  if (comparator == "==" || comparator == "eq") return value == threshold;
  if (comparator == "!=" || comparator == "ne") return value != threshold;
  return value <= threshold;
}

nlohmann::json outputPortsFromExecuteResult(const nlohmann::json& execute_result) {
  if (!execute_result.is_object()) {
    return nlohmann::json::object();
  }
  if (execute_result.contains("outputs") && execute_result.at("outputs").is_object()) {
    return execute_result.at("outputs");
  }
  if (execute_result.contains("output_ports") && execute_result.at("output_ports").is_object()) {
    return execute_result.at("output_ports");
  }
  return nlohmann::json::object();
}

nlohmann::json refObject(const nlohmann::json& value, const std::string& key) {
  if (!value.is_object() || !value.contains(key)) {
    return nlohmann::json::object();
  }
  const auto& item = value.at(key);
  if (item.is_object()) {
    return item;
  }
  if (item.is_string()) {
    return {{"uri", item.get<std::string>()}};
  }
  return nlohmann::json::object();
}

nlohmann::json carriedValueSummary(const nlohmann::json& value) {
  if (!value.is_object()) {
    return {
        {"has_carried_value", false},
        {"representation", "none"},
        {"reason", "missing_or_non_object_value"},
    };
  }

  nlohmann::json summary = {
      {"has_carried_value", true},
      {"representation", "inline_json"},
      {"contract_id", jsonString(value, "contract_id", jsonString(value, "frame_contract"))},
      {"value_kind", jsonString(value, "value_kind")},
      {"checksum", jsonString(value, "checksum")},
      {"typed_schema_id", jsonString(value, "typed_schema_id")},
      {"typed_dto_name", jsonString(value, "typed_dto_name", jsonString(value, "dto_name"))},
      {"buffer_layout_id", jsonString(value, "buffer_layout_id", jsonString(value, "layout_id"))},
  };

  const nlohmann::json typed_buffer_ref = refObject(value, "typed_buffer_ref");
  const nlohmann::json tensor_ref = refObject(value, "tensor_ref");
  const nlohmann::json artifact_ref = refObject(value, "artifact_ref");
  const nlohmann::json typed_payload_ref = refObject(value, "typed_payload_ref");
  if (!typed_buffer_ref.empty()) {
    summary["representation"] = "typed_buffer_ref";
    summary["typed_buffer_ref"] = typed_buffer_ref;
    summary["ref"] = jsonString(typed_buffer_ref, "uri", jsonString(typed_buffer_ref, "path", jsonString(typed_buffer_ref, "id")));
  } else if (!tensor_ref.empty()) {
    summary["representation"] = "tensor_ref";
    summary["tensor_ref"] = tensor_ref;
    summary["ref"] = jsonString(tensor_ref, "uri", jsonString(tensor_ref, "path", jsonString(tensor_ref, "id")));
  } else if (!artifact_ref.empty()) {
    summary["representation"] = "artifact_ref";
    summary["artifact_ref"] = artifact_ref;
    summary["ref"] = jsonString(artifact_ref, "uri", jsonString(artifact_ref, "path", jsonString(artifact_ref, "id")));
  } else if (!typed_payload_ref.empty()) {
    summary["representation"] = "typed_payload_ref";
    summary["typed_payload_ref"] = typed_payload_ref;
    summary["ref"] = jsonString(typed_payload_ref, "uri", jsonString(typed_payload_ref, "path", jsonString(typed_payload_ref, "id")));
  }

  if (value.contains("statistics") && value.at("statistics").is_object()) {
    summary["statistics"] = value.at("statistics");
  }
  if (value.contains("time_point") && value.at("time_point").is_object()) {
    summary["time_point"] = value.at("time_point");
  }
  return summary;
}

}  // namespace

nlohmann::json RuntimePublicFramePolicy::stopStatus(
    const nlohmann::json& outputs,
    int iteration_index,
    const nlohmann::json& loop_policy) {
  const int max_iterations = jsonInt(loop_policy, "max_iterations", 1);
  const double base_dt_s = jsonDouble(loop_policy, "base_dt_s", 0.0);
  const nlohmann::json stop_policy = loop_policy.value("stop_policy", nlohmann::json::object());
  const nlohmann::json alternatives = stop_policy.value("alternatives", nlohmann::json::array());
  bool stop = false;
  std::string reason;
  nlohmann::json matched = nlohmann::json::object();

  if (alternatives.is_array()) {
    for (const auto& alternative : alternatives) {
      if (!alternative.is_object()) {
        continue;
      }
      const std::string kind = jsonString(alternative, "kind");
      if ((kind == "max_steps" || kind == "max_iterations") &&
          iteration_index + 1 >= jsonInt(alternative,
                                         kind == "max_steps" ? "max_steps" : "max_iterations",
                                         max_iterations)) {
        stop = true;
        reason = jsonString(alternative, "stop_reason", kind);
        matched = alternative;
        break;
      }
      if ((kind == "max_horizon_time" || kind == "max_runtime_time") && base_dt_s > 0.0) {
        const double elapsed = (iteration_index + 1) * base_dt_s;
        const double max_time =
            jsonDouble(alternative, "max_time_s", jsonDouble(alternative, "max_runtime_time_s", 0.0));
        if (max_time > 0.0 && elapsed >= max_time) {
          stop = true;
          reason = jsonString(alternative, "stop_reason", kind);
          matched = alternative;
          matched["elapsed_time_s"] = elapsed;
          break;
        }
      }
      const nlohmann::json metric_keys = alternative.value("metric_keys", nlohmann::json::array());
      if (metric_keys.is_array() && !metric_keys.empty()) {
        std::vector<std::string> keys;
        for (const auto& key : metric_keys) {
          if (key.is_string()) {
            keys.push_back(key.get<std::string>());
          }
        }
        const double value = findNumberRecursive(outputs, keys);
        const double threshold = jsonDouble(alternative, "threshold", 0.0);
        const std::string comparator = jsonString(alternative, "comparator", "<=");
        if (compareMetric(value, comparator, threshold)) {
          stop = true;
          reason = jsonString(alternative, "stop_reason",
                              kind.empty() ? "metric_threshold_reached" : kind);
          matched = alternative;
          matched["metric_value"] = value;
          matched["threshold"] = threshold;
          break;
        }
      }
    }
  }
  if (iteration_index + 1 >= max_iterations) {
    stop = true;
    if (reason.empty()) {
      reason = "max_iterations";
    }
  }
  return {
      {"loop_iteration_index", iteration_index},
      {"stop", stop},
      {"stop_reason", reason},
      {"matched_stop_alternative", matched},
  };
}

nlohmann::json RuntimePublicFramePolicy::makeLoopStartEvent(
    const RuntimeEvent& event,
    const RuntimeLoopTick& tick,
    const nlohmann::json& external_observation,
    const std::string& timestamp_utc) {
  return {
      {"timestamp_utc", timestamp_utc},
      {"event", "loop_iteration_start"},
      {"runtime_event_id", event.event_id},
      {"runtime_event_kind", event.event_kind},
      {"status", "running"},
      {"loop_iteration_index", tick.iteration_index},
      {"time_offset_s", tick.time_offset_s},
      {"time_offset_ns", tick.time_offset.nanoseconds},
      {"public_output_time_s", tick.public_output_time_s},
      {"public_output_time_ns", tick.public_output_time.nanoseconds},
      {"output_period_s", tick.output_period_s},
      {"output_period_ns", tick.output_period.nanoseconds},
      {"external_observation", external_observation},
  };
}

nlohmann::json RuntimePublicFramePolicy::makeLoopFinishEvent(
    const RuntimeEvent& event,
    const RuntimeLoopTick& tick,
    const nlohmann::json& stop_status,
    int held_output_count,
    int public_tick_failed_nodes,
    double output_period_s,
    const std::string& timestamp_utc) {
  return {
      {"timestamp_utc", timestamp_utc},
      {"event", "loop_iteration_finish"},
      {"runtime_event_id", event.event_id},
      {"runtime_event_kind", event.event_kind},
      {"status", public_tick_failed_nodes > 0 ? "failed" : "ok"},
      {"loop_iteration_index", tick.iteration_index},
      {"public_output_time_s", tick.public_output_time_s},
      {"public_output_time_ns", tick.public_output_time.nanoseconds},
      {"output_period_s", output_period_s},
      {"output_period_ns", tick.output_period.nanoseconds},
      {"held_output_count", held_output_count},
      {"stop", stop_status.value("stop", false)},
      {"stop_reason", stop_status.value("stop_reason", "")},
  };
}

nlohmann::json RuntimePublicFramePolicy::outputPortPolicy(
    const nlohmann::json& node,
    const nlohmann::json& data_plane_info,
    bool has_output,
    const std::string& reason,
    const nlohmann::json& last_execute_result,
    const nlohmann::json& port_store_refs) {
  nlohmann::json ports = nlohmann::json::array();
  const nlohmann::json output_specs = data_plane_info.value("outputs", nlohmann::json::array());
  const nlohmann::json last_ports = outputPortsFromExecuteResult(last_execute_result);
  const nlohmann::json store_ports = port_store_refs.value("ports", nlohmann::json::object());
  if (output_specs.is_array()) {
    for (const auto& spec : output_specs) {
      if (!spec.is_object()) {
        continue;
      }
      const std::string port_id = jsonString(spec, "port_id");
      const bool has_port_value = has_output && last_ports.is_object() && !port_id.empty() && last_ports.contains(port_id);
      const bool has_store_value = has_output && store_ports.is_object() && !port_id.empty() &&
                                   store_ports.contains(port_id) && store_ports.at(port_id).is_object() &&
                                   store_ports.at(port_id).value("has_carried_value", false);
      nlohmann::json carried_value =
          has_store_value ? store_ports.at(port_id)
                          : (has_port_value ? carriedValueSummary(last_ports.at(port_id))
                                            : nlohmann::json{{"has_carried_value", false},
                                                             {"representation", "none"},
                                                             {"reason", has_output ? "port_not_found_in_last_output" : "no_value_until_first_output"}});
      if (has_store_value) {
        carried_value["preferred_source"] = "ThreadSafePortStore";
      }
      ports.push_back({
          {"node_id", jsonString(node, "node_id")},
          {"operator_id", jsonString(node, "operator_id")},
          {"port_id", port_id},
          {"contract_id", jsonString(spec, "contract_id")},
          {"typed_io_contract", spec.value("typed_io_contract", nlohmann::json::object())},
          {"hold_strategy", has_output ? "carry_forward_last_output" : "no_value_until_first_output"},
          {"held_reason", reason},
          {"has_output", has_output},
          {"has_carried_value", carried_value.value("has_carried_value", false)},
          {"carried_value", carried_value},
      });
    }
  }
  if (ports.empty()) {
    const nlohmann::json carried_value =
        has_output ? carriedValueSummary(last_execute_result)
                   : nlohmann::json{{"has_carried_value", false},
                                    {"representation", "none"},
                                    {"reason", "no_value_until_first_output"}};
    const bool has_carried_value = carried_value.value("has_carried_value", false);
    ports.push_back({
        {"node_id", jsonString(node, "node_id")},
        {"operator_id", jsonString(node, "operator_id")},
        {"port_id", ""},
        {"contract_id", ""},
        {"hold_strategy", has_output ? "carry_forward_last_output" : "no_value_until_first_output"},
        {"held_reason", reason},
        {"has_output", has_output},
        {"has_carried_value", has_carried_value},
        {"carried_value", carried_value},
    });
  }
  return ports;
}

nlohmann::json RuntimePublicFramePolicy::makeHeldOutput(const RuntimeHeldOutputRequest& request) {
  const nlohmann::json empty = nlohmann::json::object();
  const nlohmann::json& node = request.node != nullptr ? *request.node : empty;
  const nlohmann::json& data_plane = request.data_plane_info != nullptr ? *request.data_plane_info : empty;
  const int iteration = request.tick != nullptr ? request.tick->iteration_index : 0;
  const double public_time_s = request.tick != nullptr ? request.tick->public_output_time_s : 0.0;
  const long long public_time_ns = request.tick != nullptr ? request.tick->public_output_time.nanoseconds : 0;

  nlohmann::json held = {
      {"node_id", jsonString(node, "node_id")},
      {"operator_id", jsonString(node, "operator_id")},
      {"loop_iteration_index", iteration},
      {"reason", request.reason},
      {"has_output", request.has_output},
      {"public_output_time_s", public_time_s},
      {"public_output_time_ns", public_time_ns},
      {"ports", outputPortPolicy(node,
                                  data_plane,
                                  request.has_output,
                                  request.reason,
                                  request.state != nullptr ? request.state->last_execute_result : empty,
                                  request.port_store_refs != nullptr ? *request.port_store_refs : empty)},
  };
  if (request.state != nullptr) {
    held["held_from_loop_iteration_index"] = request.state->last_execution_iteration;
    held["last_time_info"] = request.state->last_time_info;
  }
  return held;
}

nlohmann::json RuntimePublicFramePolicy::makeHeldSchedulerEvent(const RuntimeHeldOutputRequest& request) {
  const nlohmann::json empty = nlohmann::json::object();
  const nlohmann::json& node = request.node != nullptr ? *request.node : empty;
  const int iteration = request.tick != nullptr ? request.tick->iteration_index : 0;
  const double public_time_s = request.tick != nullptr ? request.tick->public_output_time_s : 0.0;
  const long long public_time_ns = request.tick != nullptr ? request.tick->public_output_time.nanoseconds : 0;

  nlohmann::json event = {
      {"timestamp_utc", request.timestamp_utc},
      {"node_id", jsonString(node, "node_id")},
      {"event", "held"},
      {"runtime_event_id", request.event != nullptr ? request.event->event_id : ""},
      {"runtime_event_kind", request.event != nullptr ? request.event->event_kind : ""},
      {"status", "held"},
      {"reason", request.reason},
      {"dispatch_tick_index", request.dispatch_tick_index},
      {"dispatch_time_s", public_time_s},
      {"dispatch_time_ns", public_time_ns},
      {"loop_iteration_index", iteration},
      {"ports", outputPortPolicy(node,
                                  request.data_plane_info != nullptr ? *request.data_plane_info : empty,
                                  request.has_output,
                                  request.reason,
                                  request.state != nullptr ? request.state->last_execute_result : empty,
                                  request.port_store_refs != nullptr ? *request.port_store_refs : empty)},
  };
  if (request.state != nullptr) {
    event["held_from_loop_iteration_index"] = request.state->last_execution_iteration;
    event["last_time_info"] = request.state->last_time_info;
  }
  return event;
}

void RuntimePublicFramePolicy::decorateStopStatus(
    nlohmann::json& stop_status,
    const RuntimeEvent& event,
    const RuntimeLoopTick& tick,
    const nlohmann::json& effective_delta_t_s_by_node,
    const nlohmann::json& held_outputs,
    int public_tick_failed_nodes,
    double base_dt_s,
    double output_period_s,
    int node_event_count) {
  stop_status["failed_nodes"] = public_tick_failed_nodes;
  stop_status["run_time_s"] = tick.public_output_time_s;
  stop_status["public_output_time_s"] = tick.public_output_time_s;
  stop_status["public_output_time_ns"] = tick.public_output_time.nanoseconds;
  stop_status["output_period_s"] = output_period_s;
  stop_status["output_period_ns"] = tick.output_period.nanoseconds;
  stop_status["base_dt_s"] = base_dt_s;
  stop_status["base_dt_ns"] = tick.base_dt.nanoseconds;
  stop_status["effective_delta_t_s_by_node"] = effective_delta_t_s_by_node;
  stop_status["event_scheduler"] = {
      {"mode", "event_queue"},
      {"public_event_id", event.event_id},
      {"public_event_time_s", event.event_time_s},
      {"public_event_time_ns", event.event_time.nanoseconds},
      {"node_event_count", node_event_count},
  };
  stop_status["held_outputs"] = held_outputs;
  stop_status["held_output_count"] = held_outputs.size();
}

}  // namespace FlightEnvPlatformRuntime
