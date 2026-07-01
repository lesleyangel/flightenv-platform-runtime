#include "FlightEnvPlatformRuntime/RuntimeRateTransitionExecutor.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeInputAlignment.hpp"

#include "FlightEnvPlatform/Runtime/ThreadSafePortStore.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using FlightEnvPlatformRuntime::RuntimeInputAlignment;
using FlightEnvPlatformRuntime::RuntimePortSample;
using FlightEnvPlatformRuntime::RuntimePortSampleBuffer;
using FlightEnvPlatformRuntime::RuntimeRateTransitionExecutionRequest;
using FlightEnvPlatformRuntime::RuntimeRateTransitionExecutor;
using json = nlohmann::json;

struct AuditContext {
  json cases = json::array();
  int failures = 0;

  void record(const std::string& name, bool passed, const std::string& message, json evidence = json::object()) {
    cases.push_back({
        {"name", name},
        {"status", passed ? "PASS" : "FAIL"},
        {"message", message},
        {"evidence", std::move(evidence)},
    });
    if (!passed) {
      ++failures;
      std::cerr << "[FAIL] " << name << ": " << message << "\n";
    } else {
      std::cout << "[PASS] " << name << "\n";
    }
  }
};

bool nearly(double actual, double expected, double tolerance = 1.0e-9) {
  return std::abs(actual - expected) <= tolerance;
}

RuntimePortSample sample(
    const std::string& node_id,
    const std::string& port_id,
    double time_s,
    json value) {
  RuntimePortSample out;
  out.channel_id = RuntimePortSampleBuffer::portChannel(node_id, port_id);
  out.source_node_id = node_id;
  out.source_port_id = port_id;
  out.time_s = time_s;
  out.iteration_index = static_cast<int>(std::round(time_s * 10.0));
  out.value = std::move(value);
  out.time_info = {
      {"public_output_time_s", time_s},
      {"runtime_event", {{"event_id", "evt." + node_id + "." + port_id + "." + std::to_string(out.iteration_index)}}},
  };
  return out;
}

RuntimePortSampleBuffer seedBuffer() {
  RuntimePortSampleBuffer buffer;
  buffer.recordSample(sample("src", "value", 0.0, 0.0));
  buffer.recordSample(sample("src", "value", 1.0, 10.0));
  buffer.recordSample(sample("src", "value", 1.5, 15.0));
  buffer.recordSample(sample("src", "value", 2.0, 20.0));
  return buffer;
}

json timeInfo(double target_time_s, double effective_delta_s = 1.0) {
  return {
      {"public_output_time_s", target_time_s},
      {"effective_delta_t_s", effective_delta_s},
      {"output_time_point",
       {
           {"run_time_s", target_time_s},
           {"tick_index", static_cast<int>(std::round(target_time_s * 10.0))},
           {"source_time_s", target_time_s},
           {"stamp_ns", static_cast<double>(target_time_s * 1000000000.0)},
       }},
      {"runtime_event", {{"event_id", "evt.target." + std::to_string(static_cast<int>(target_time_s * 10.0))}}},
  };
}

json transition(
    const std::string& suffix,
    const std::string& strategy,
    const std::string& target_port_id) {
  return {
      {"transition_id", "rate_transition." + suffix},
      {"binding_id", "binding." + suffix},
      {"source_node_id", "src"},
      {"source_port_id", "value"},
      {"target_node_id", "target"},
      {"target_port_id", target_port_id},
      {"source_period_s", 0.5},
      {"target_period_s", 1.0},
      {"rate_relation", "fast_to_slow"},
      {"strategy", strategy},
      {"alignment", strategy},
      {"input_resampling", strategy},
      {"requires_runtime_transition", true},
      {"insertion_mode", "runtime_synthetic_node"},
      {"transition_node_id", "platform.rate_transition." + suffix},
      {"max_age_s", 5.0},
  };
}

json targetNode() {
  return {
      {"node_id", "target"},
      {"rate_transitions",
       json::array({
           transition("hold", "hold", "input.hold"),
           transition("interpolate", "interpolate", "input.interpolate"),
           transition("window", "window", "input.window"),
           transition("filter", "filter_then_decimate", "input.filter"),
       })},
  };
}

json missingSourceNode() {
  return {
      {"node_id", "missing_target"},
      {"rate_transitions",
       json::array({
           {
               {"transition_id", "rate_transition.missing"},
               {"binding_id", "binding.missing"},
               {"source_node_id", "missing_src"},
               {"source_port_id", "value"},
               {"target_node_id", "missing_target"},
               {"target_port_id", "input.missing"},
               {"source_period_s", 0.5},
               {"target_period_s", 1.0},
               {"rate_relation", "fast_to_slow"},
               {"strategy", "hold"},
               {"alignment", "hold"},
               {"input_resampling", "hold"},
               {"requires_runtime_transition", true},
               {"insertion_mode", "runtime_synthetic_node"},
               {"transition_node_id", "platform.rate_transition.missing"},
               {"max_age_s", 0.1},
           },
       })},
  };
}

double inputPortScalar(const json& upstream, const std::string& port_id) {
  return upstream.at("input_ports").at(port_id).get<double>();
}

void caseSyntheticExecution(AuditContext& audit) {
  RuntimePortSampleBuffer buffer = seedBuffer();
  flightenv::platform::ThreadSafePortStore port_store;
  const json node = targetNode();
  const json time_info = timeInfo(1.5, 1.0);

  const auto result = RuntimeRateTransitionExecutor::executeForTarget({
      "phase9.audit",
      "object.audit",
      "branch.audit",
      "timeline.audit",
      "target",
      node,
      time_info,
      15,
      &buffer,
      &port_store,
  });

  const auto hold_sample =
      buffer.latestBeforeOrAt(RuntimePortSampleBuffer::portChannel("platform.rate_transition.hold", "input.hold"), 1.5);
  const auto filter_packet =
      port_store.read_node_port_output_scoped(
          "branch.audit",
          "timeline.audit",
          "platform.rate_transition.filter",
          "input.filter");

  const bool ok =
      result.transition_count == 4 &&
      result.executed_count == 4 &&
      result.blocked_count == 0 &&
      result.packet_count == 4 &&
      hold_sample.has_value() &&
      hold_sample->value.is_number() &&
      nearly(hold_sample->value.get<double>(), 15.0) &&
      filter_packet.has_value();

  audit.record(
      "synthetic_rate_transition_nodes_write_samples_and_packets",
      ok,
      "runtime executes transition nodes and writes both sample-buffer and port-store outputs",
      {
          {"result",
           {
               {"transition_count", result.transition_count},
               {"executed_count", result.executed_count},
               {"blocked_count", result.blocked_count},
               {"packet_count", result.packet_count},
               {"sample_count", result.sample_count},
           }},
          {"events", result.events},
          {"hold_sample", hold_sample.has_value() ? hold_sample->evidence() : json::object()},
          {"filter_packet_present", filter_packet.has_value()},
      });

  const auto alignment = RuntimeInputAlignment::alignNodeInputs(node, time_info, buffer);
  json upstream = json::object();
  RuntimeInputAlignment::applyAlignedInputs(alignment, upstream);
  const bool alignment_ok =
      alignment.inputs.size() == 4 &&
      !alignment.hasUnavailableRequiredInputs() &&
      upstream.contains("input_ports") &&
      nearly(inputPortScalar(upstream, "input.hold"), 15.0) &&
      nearly(inputPortScalar(upstream, "input.interpolate"), 15.0) &&
      upstream.at("input_ports").at("input.window").value("representation", "") == "runtime_window_samples" &&
      nearly(inputPortScalar(upstream, "input.filter"), 12.5);
  audit.record(
      "target_node_consumes_synthetic_transition_outputs",
      alignment_ok,
      "target input alignment reads transition_node_id outputs instead of direct upstream samples",
      {
          {"alignment", alignment.evidence()},
          {"upstream", upstream},
      });
}

void caseMissingSourceBlocks(AuditContext& audit) {
  RuntimePortSampleBuffer buffer = seedBuffer();
  flightenv::platform::ThreadSafePortStore port_store;
  const json node = missingSourceNode();
  const json time_info = timeInfo(1.5, 1.0);

  const auto result = RuntimeRateTransitionExecutor::executeForTarget({
      "phase9.audit",
      "object.audit",
      "branch.audit",
      "timeline.audit",
      "missing_target",
      node,
      time_info,
      15,
      &buffer,
      &port_store,
  });
  const auto missing_sample =
      buffer.latestBeforeOrAt(
          RuntimePortSampleBuffer::portChannel("platform.rate_transition.missing", "input.missing"),
          1.5);
  const bool ok =
      result.transition_count == 1 &&
      result.executed_count == 0 &&
      result.blocked_count == 1 &&
      !missing_sample.has_value() &&
      !result.unavailable_transitions.empty();
  audit.record(
      "missing_transition_source_blocks_without_output_packet",
      ok,
      "missing source input blocks the synthetic node and does not publish a stale output",
      {
          {"events", result.events},
          {"unavailable_transitions", result.unavailable_transitions},
      });
}

}  // namespace

int main(int argc, char** argv) {
  const std::string report_path =
      argc > 1 ? argv[1] : "_local_artifacts/platform-runtime/rate-transition-synthetic-phase9-audit.json";

  AuditContext audit;
  caseSyntheticExecution(audit);
  caseMissingSourceBlocks(audit);

  const json report = {
      {"schema_version", "flightenv.platform.rate_transition_synthetic_phase9_audit.v1"},
      {"status", audit.failures == 0 ? "passed" : "failed"},
      {"summary",
       {
           {"case_count", audit.cases.size()},
           {"failure_count", audit.failures},
           {"covered_strategies", {"hold", "interpolate", "window", "filter_then_decimate"}},
           {"synthetic_node_packet_gate_covered", true},
           {"target_consumes_transition_node_output", true},
       }},
      {"cases", audit.cases},
  };

  std::ofstream output(report_path, std::ios::binary);
  if (!output) {
    std::cerr << "Cannot write report: " << report_path << "\n";
    return 2;
  }
  output << report.dump(2) << "\n";
  return audit.failures == 0 ? 0 : 1;
}
