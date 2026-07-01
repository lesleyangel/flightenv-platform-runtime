#include "FlightEnvPlatformRuntime/RuntimeRateTransitionExecutor.hpp"

#include "FlightEnvPlatformRuntime/RuntimePortPacketWriter.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeInputAlignment.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <chrono>
#include <ctime>
#include <optional>
#include <sstream>
#include <utility>

namespace FlightEnvPlatformRuntime {
namespace {

std::string nowUtcIso() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const std::time_t time = clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

std::string jsonString(
    const nlohmann::json& value,
    const std::string& key,
    const std::string& fallback = "") {
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

bool jsonBool(const nlohmann::json& value, const std::string& key, bool fallback = false) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean()) {
    return fallback;
  }
  return value.at(key).get<bool>();
}

std::string stableId(std::string text) {
  std::replace_if(text.begin(), text.end(), [](unsigned char c) {
    return !(std::isalnum(c) || c == '_' || c == '-' || c == '.');
  }, '_');
  return text;
}

double targetTimeS(const nlohmann::json& time_info) {
  return jsonDouble(
      time_info,
      "public_output_time_s",
      jsonDouble(time_info.value("output_time_point", nlohmann::json::object()), "run_time_s", 0.0));
}

double effectiveDeltaS(const nlohmann::json& time_info) {
  return jsonDouble(time_info, "effective_delta_t_s", jsonDouble(time_info, "delta_t_s", 0.0));
}

std::optional<double> scalarValue(const nlohmann::json& value) {
  if (value.is_number()) {
    return value.get<double>();
  }
  if (value.is_object() && value.contains("value") && value.at("value").is_number()) {
    return value.at("value").get<double>();
  }
  return std::nullopt;
}

nlohmann::json scalarLike(const nlohmann::json& original, double value) {
  if (original.is_object()) {
    nlohmann::json patched = original;
    patched["value"] = value;
    return patched;
  }
  return value;
}

std::string sourceAlignmentStrategy(const std::string& strategy) {
  if (strategy == "hold" || strategy == "hold_last" || strategy == "zero_order_hold" ||
      strategy == "invalid_if_stale") {
    return "hold";
  }
  if (strategy == "linear_interpolation") {
    return "interpolate";
  }
  if (strategy == "decimate") {
    return "latest_before";
  }
  return strategy;
}

bool requiresSyntheticTransition(const nlohmann::json& transition) {
  if (!transition.is_object()) {
    return false;
  }
  if (!jsonBool(transition, "requires_runtime_transition", false)) {
    return false;
  }
  return !jsonString(transition, "transition_node_id").empty();
}

nlohmann::json rateTransitionEvidence(const nlohmann::json& transition) {
  if (!transition.is_object()) {
    return nlohmann::json::object();
  }
  nlohmann::json out = {
      {"transition_id", jsonString(transition, "transition_id")},
      {"binding_id", jsonString(transition, "binding_id")},
      {"transition_node_id", jsonString(transition, "transition_node_id")},
      {"source_node_id", jsonString(transition, "source_node_id")},
      {"source_port_id", jsonString(transition, "source_port_id")},
      {"target_node_id", jsonString(transition, "target_node_id")},
      {"target_port_id", jsonString(transition, "target_port_id")},
      {"rate_relation", jsonString(transition, "rate_relation")},
      {"strategy", jsonString(transition, "strategy")},
      {"source_period_s", transition.value("source_period_s", -1.0)},
      {"target_period_s", transition.value("target_period_s", -1.0)},
      {"requires_runtime_transition", jsonBool(transition, "requires_runtime_transition", false)},
      {"insertion_mode", jsonString(transition, "insertion_mode")},
  };
  if (transition.contains("max_age_s")) {
    out["max_age_s"] = transition.at("max_age_s");
  }
  return out;
}

nlohmann::json sourceAlignmentPolicy(const nlohmann::json& transition) {
  nlohmann::json policy = transition;
  const std::string strategy = sourceAlignmentStrategy(jsonString(transition, "strategy", "latest_before"));
  policy.erase("transition_node_id");
  policy["strategy"] = strategy;
  policy["alignment"] = strategy == "aggregate" ? "window" : strategy;
  policy["input_resampling"] = strategy;
  policy["requires_runtime_transition"] = true;
  return policy;
}

nlohmann::json sourceAlignmentNode(const nlohmann::json& transition) {
  return {
      {"node_id", jsonString(transition, "transition_node_id") + ".source_alignment"},
      {"rate_transitions", nlohmann::json::array({sourceAlignmentPolicy(transition)})},
  };
}

nlohmann::json transitionTimeInfo(
    const nlohmann::json& base_time_info,
    const nlohmann::json& transition,
    int iteration_index) {
  nlohmann::json out = base_time_info.is_object() ? base_time_info : nlohmann::json::object();
  const double target_time_s = targetTimeS(out);
  const std::string base_event_id =
      jsonString(out.value("runtime_event", nlohmann::json::object()), "event_id",
                 "runtime_event." + std::to_string(iteration_index));
  out["public_output_time_s"] = target_time_s;
  out["public_output_time_ns"] =
      RuntimeTimePoint::fromSeconds(target_time_s).nanoseconds;
  out["runtime_event"] = {
      {"event_id", base_event_id + ".synthetic." + stableId(jsonString(transition, "transition_id"))},
      {"event_kind", "rate_transition_synthetic_node"},
      {"transition_id", jsonString(transition, "transition_id")},
      {"transition_node_id", jsonString(transition, "transition_node_id")},
  };
  out["producer"] = "RuntimeRateTransitionExecutor";
  out["synthetic_rate_transition"] = true;
  return out;
}

void retargetOutputValue(nlohmann::json& value, const std::string& target_port_id) {
  if (!value.is_object() || target_port_id.empty()) {
    return;
  }
  const std::string source_port_id = jsonString(value, "port_id");
  if (!source_port_id.empty() && !value.contains("source_port_id")) {
    value["source_port_id"] = source_port_id;
  }
  value["port_id"] = target_port_id;
  if (value.contains("typed_buffer_ref") && value.at("typed_buffer_ref").is_object()) {
    nlohmann::json& typed_ref = value["typed_buffer_ref"];
    const std::string typed_source_port_id = jsonString(typed_ref, "port_id", source_port_id);
    if (!typed_source_port_id.empty() && !typed_ref.contains("source_port_id")) {
      typed_ref["source_port_id"] = typed_source_port_id;
    }
    typed_ref["port_id"] = target_port_id;
  }
}

std::string sourceChannel(const nlohmann::json& transition) {
  return RuntimePortSampleBuffer::portChannel(
      jsonString(transition, "source_node_id"),
      jsonString(transition, "source_port_id"));
}

struct TransitionValue {
  bool available = false;
  std::string status = "not_evaluated";
  nlohmann::json value = nlohmann::json();
  nlohmann::json evidence = nlohmann::json::object();
  int sample_count = 0;
};

TransitionValue evaluateWindowNumeric(
    const nlohmann::json& transition,
    const nlohmann::json& time_info,
    const RuntimePortSampleBuffer& sample_buffer,
    const std::string& operation) {
  TransitionValue result;
  const double target_time_s = targetTimeS(time_info);
  const double window_start_s = target_time_s - std::max(0.0, effectiveDeltaS(time_info));
  const std::vector<RuntimePortSample> samples =
      sample_buffer.window(sourceChannel(transition), window_start_s, target_time_s);
  result.sample_count = static_cast<int>(samples.size());
  nlohmann::json sample_evidence = nlohmann::json::array();
  double sum = 0.0;
  double sum_sq = 0.0;
  bool all_numeric = !samples.empty();
  nlohmann::json representative = samples.empty() ? nlohmann::json() : samples.back().value;
  for (const auto& sample : samples) {
    sample_evidence.push_back(sample.evidence());
    const auto scalar = scalarValue(sample.value);
    if (!scalar.has_value()) {
      all_numeric = false;
      continue;
    }
    sum += *scalar;
    sum_sq += (*scalar) * (*scalar);
  }
  if (samples.empty()) {
    result.status = "insufficient_samples";
  } else if (!all_numeric) {
    result.status = "unsupported_value_kind";
  } else {
    const double count = static_cast<double>(samples.size());
    const double mean = sum / count;
    const double rms = std::sqrt(sum_sq / count);
    if (operation == "window_rms") {
      result.value = scalarLike(representative, rms);
    } else if (operation == "feature_extract") {
      result.value = {
          {"representation", "runtime_rate_transition_features"},
          {"sample_count", samples.size()},
          {"window_start_s", window_start_s},
          {"window_end_s", target_time_s},
          {"mean", mean},
          {"rms", rms},
      };
    } else {
      result.value = scalarLike(representative, mean);
    }
    result.available = true;
    result.status = operation;
  }
  result.evidence = {
      {"operation", operation},
      {"status", result.status},
      {"available", result.available},
      {"window_start_s", window_start_s},
      {"window_end_s", target_time_s},
      {"sample_count", samples.size()},
      {"samples", sample_evidence},
  };
  if (operation == "filter_then_decimate") {
    result.evidence["filter_semantics"] = "deterministic_window_mean";
  }
  return result;
}

TransitionValue evaluateSource(
    const nlohmann::json& transition,
    const nlohmann::json& time_info,
    const RuntimePortSampleBuffer& sample_buffer) {
  const std::string strategy = jsonString(transition, "strategy", "latest_before");
  if (strategy == "window_mean" || strategy == "window_rms" ||
      strategy == "filter_then_decimate" || strategy == "feature_extract") {
    return evaluateWindowNumeric(transition, time_info, sample_buffer, strategy);
  }

  TransitionValue result;
  const RuntimeInputAlignmentResult alignment =
      RuntimeInputAlignment::alignNodeInputs(sourceAlignmentNode(transition), time_info, sample_buffer);
  result.evidence = alignment.evidence();
  if (alignment.inputs.empty()) {
    result.status = "no_alignment_policy";
    return result;
  }
  const RuntimeAlignedInput& input = alignment.inputs.front();
  result.available = input.available;
  result.status = input.status;
  result.sample_count = input.sample_count;
  result.value = input.value;
  return result;
}

nlohmann::json outputDataPlaneEntry(
    const nlohmann::json& transition,
    const nlohmann::json& value) {
  return {
      {"direction", "output"},
      {"port_id", jsonString(transition, "target_port_id")},
      {"role", "rate_transition_output"},
      {"representation", "runtime_rate_transition_inline"},
      {"value_kind", value.type_name()},
      {"transition_id", jsonString(transition, "transition_id")},
      {"transition_node_id", jsonString(transition, "transition_node_id")},
      {"source_node_id", jsonString(transition, "source_node_id")},
      {"source_port_id", jsonString(transition, "source_port_id")},
  };
}

nlohmann::json executeResult(
    const nlohmann::json& transition,
    const nlohmann::json& value,
    const TransitionValue& source) {
  const std::string target_port_id = jsonString(transition, "target_port_id");
  return {
      {"schema_version", "flightenv.platform.runtime_rate_transition_node_result.v1"},
      {"status", "ok"},
      {"node_id", jsonString(transition, "transition_node_id")},
      {"operator_id", "platform.rate_transition.synthetic.v1"},
      {"synthetic_node", true},
      {"outputs", {{target_port_id, value}}},
      {"rate_transition", rateTransitionEvidence(transition)},
      {"source_alignment", source.evidence},
  };
}

nlohmann::json baseEvent(
    const nlohmann::json& transition,
    const std::string& target_node_id,
    const nlohmann::json& time_info,
    int iteration_index) {
  return {
      {"timestamp_utc", nowUtcIso()},
      {"event", "rate_transition_synthetic_node"},
      {"status", "pending"},
      {"target_node_id", target_node_id},
      {"transition_id", jsonString(transition, "transition_id")},
      {"transition_node_id", jsonString(transition, "transition_node_id")},
      {"binding_id", jsonString(transition, "binding_id")},
      {"strategy", jsonString(transition, "strategy")},
      {"rate_relation", jsonString(transition, "rate_relation")},
      {"runtime_event_time_s", targetTimeS(time_info)},
      {"runtime_event_time_ns", RuntimeTimePoint::fromSeconds(targetTimeS(time_info)).nanoseconds},
      {"loop_iteration_index", iteration_index},
      {"rate_transition", rateTransitionEvidence(transition)},
  };
}

}  // namespace

bool RuntimeRateTransitionExecutionResult::hasEvidence() const {
  return transition_count > 0 || !events.empty();
}

RuntimeRateTransitionExecutionResult RuntimeRateTransitionExecutor::executeForTarget(
    const RuntimeRateTransitionExecutionRequest& request) {
  RuntimeRateTransitionExecutionResult result;
  if (request.sample_buffer == nullptr || !request.target_node.is_object()) {
    return result;
  }
  const nlohmann::json transitions =
      request.target_node.value("rate_transitions", nlohmann::json::array());
  if (!transitions.is_array()) {
    return result;
  }
  for (const auto& transition : transitions) {
    if (!requiresSyntheticTransition(transition)) {
      continue;
    }
    ++result.transition_count;
    nlohmann::json event =
        baseEvent(transition, request.target_node_id, request.time_info, request.iteration_index);
    const TransitionValue source = evaluateSource(transition, request.time_info, *request.sample_buffer);
    event["source_alignment"] = source.evidence;
    event["source_status"] = source.status;
    event["source_available"] = source.available;
    event["sample_count"] = source.sample_count;
    if (!source.available) {
      ++result.blocked_count;
      event["status"] = "blocked";
      event["reason"] = "source_unavailable";
      result.unavailable_transitions.push_back(event);
      result.events.push_back(event);
      continue;
    }

    nlohmann::json output_value = source.value;
    retargetOutputValue(output_value, jsonString(transition, "target_port_id"));
    const nlohmann::json synthetic_time_info =
        transitionTimeInfo(request.time_info, transition, request.iteration_index);
    const nlohmann::json synthetic_result = executeResult(transition, output_value, source);
    request.sample_buffer->recordNodeOutput(
        jsonString(transition, "transition_node_id"),
        synthetic_result,
        synthetic_time_info,
        request.iteration_index);
    ++result.sample_count;
    result.outputs_by_transition_node[jsonString(transition, "transition_node_id")] = synthetic_result;

    nlohmann::json packet_summaries = nlohmann::json::array();
    if (request.port_store != nullptr) {
      RuntimePortPacketWriteRequest packet_request;
      packet_request.run_id = request.run_id;
      packet_request.object_id = request.object_id;
      packet_request.branch_id = request.branch_id;
      packet_request.timeline_id = request.timeline_id;
      packet_request.node_id = jsonString(transition, "transition_node_id");
      packet_request.execute_result = synthetic_result;
      packet_request.time_info = synthetic_time_info;
      packet_request.data_plane_entries =
          nlohmann::json::array({outputDataPlaneEntry(transition, output_value)});
      const RuntimePortPacketWriteResult packet_result =
          RuntimePortPacketWriter::writeOutputPortPackets(*request.port_store, packet_request);
      packet_summaries = packet_result.packets;
      for (const auto& packet : packet_result.packets) {
        result.packets.push_back(packet);
      }
      result.packet_count += static_cast<int>(packet_result.packets.size());
    }

    ++result.executed_count;
    event["status"] = "executed";
    event["reason"] = "synthetic_transition_output_written";
    event["synthetic_output"] = synthetic_result;
    event["runtime_port_packets"] = packet_summaries;
    result.events.push_back(event);
  }
  return result;
}

}  // namespace FlightEnvPlatformRuntime
