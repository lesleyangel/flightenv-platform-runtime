#include "FlightEnvPlatformRuntime/time/RuntimeInputAlignment.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using FlightEnvPlatformRuntime::RuntimeInputAlignment;
using FlightEnvPlatformRuntime::RuntimeInputAlignmentResult;
using FlightEnvPlatformRuntime::RuntimePortSample;
using FlightEnvPlatformRuntime::RuntimePortSampleBuffer;
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
  buffer.recordSample(sample("src", "value", 2.0, 20.0));
  buffer.recordSample(sample("stale_src", "value", 0.0, 5.0));
  return buffer;
}

json timeInfo(double target_time_s, double effective_delta_s = 1.0) {
  return {
      {"public_output_time_s", target_time_s},
      {"effective_delta_t_s", effective_delta_s},
      {"runtime_event", {{"event_id", "evt.target." + std::to_string(static_cast<int>(target_time_s * 10.0))}}},
  };
}

json nodeWithPolicy(
    const std::string& strategy,
    const std::string& target_port_id,
    const std::string& source_node_id = "src",
    double max_age_s = -1.0) {
  json policy = {
      {"transition_id", "transition." + strategy + "." + target_port_id},
      {"binding_id", "binding." + strategy + "." + target_port_id},
      {"rate_relation", "slow_to_fast"},
      {"requires_runtime_transition", true},
      {"strategy", strategy},
      {"alignment", strategy},
      {"input_resampling", strategy == "window" ? "window" : strategy},
      {"upstream_node_id", source_node_id},
      {"source_port_id", "value"},
      {"target_port_id", target_port_id},
      {"source_period_s", 1.0},
      {"target_period_s", 0.5},
  };
  if (max_age_s >= 0.0) {
    policy["max_age_s"] = max_age_s;
  }
  return {
      {"node_id", "target." + strategy},
      {"rate_transitions", json::array({policy})},
  };
}

json firstEvidence(const RuntimeInputAlignmentResult& result) {
  if (result.inputs.empty()) {
    return json::object();
  }
  return result.inputs.front().evidence();
}

bool nearly(double actual, double expected, double tolerance = 1.0e-9) {
  return std::abs(actual - expected) <= tolerance;
}

RuntimeInputAlignmentResult align(
    const RuntimePortSampleBuffer& buffer,
    const std::string& strategy,
    double target_time_s,
    double effective_delta_s = 1.0,
    const std::string& source_node_id = "src",
    double max_age_s = -1.0) {
  return RuntimeInputAlignment::alignNodeInputs(
      nodeWithPolicy(strategy, "target." + strategy, source_node_id, max_age_s),
      timeInfo(target_time_s, effective_delta_s),
      buffer);
}

void caseExact(AuditContext& audit, const RuntimePortSampleBuffer& buffer) {
  const auto result = align(buffer, "exact", 1.0);
  const bool ok =
      result.inputs.size() == 1 &&
      result.inputs.front().available &&
      result.inputs.front().status == "exact" &&
      result.inputs.front().value.is_number() &&
      nearly(result.inputs.front().value.get<double>(), 10.0) &&
      firstEvidence(result).value("strategy", "") == "exact" &&
      firstEvidence(result)
          .value("temporal_port_contract", json::object())
          .value("producer_event_id", "") == "evt.src.value.10";
  audit.record("exact_requires_timestamp_match", ok, "exact sample at target time is selected", firstEvidence(result));
}

void caseLatestBefore(AuditContext& audit, const RuntimePortSampleBuffer& buffer) {
  const auto result = align(buffer, "latest_before", 1.5);
  const bool ok =
      result.inputs.size() == 1 &&
      result.inputs.front().available &&
      result.inputs.front().status == "latest_before" &&
      nearly(result.inputs.front().source_time_s, 1.0) &&
      firstEvidence(result).value("strategy", "") == "latest_before";
  audit.record("latest_before_selects_prior_sample", ok, "latest_before uses the closest sample not after target", firstEvidence(result));
}

void caseHold(AuditContext& audit, const RuntimePortSampleBuffer& buffer) {
  const auto result = align(buffer, "hold", 1.5, 1.0, "src", 1.0);
  const bool ok =
      result.inputs.size() == 1 &&
      result.inputs.front().available &&
      result.inputs.front().status == "hold_last" &&
      nearly(result.inputs.front().source_time_s, 1.0) &&
      firstEvidence(result).value("raw_strategy", "") == "hold";
  audit.record("hold_alias_maps_to_hold_last", ok, "hold alias produces hold_last runtime behavior", firstEvidence(result));
}

void caseInterpolate(AuditContext& audit, const RuntimePortSampleBuffer& buffer) {
  const auto result = align(buffer, "interpolate", 1.5);
  const bool ok =
      result.inputs.size() == 1 &&
      result.inputs.front().available &&
      result.inputs.front().status == "linear" &&
      result.inputs.front().value.is_number() &&
      nearly(result.inputs.front().value.get<double>(), 15.0);
  audit.record("interpolate_computes_scalar_linear_value", ok, "interpolate uses bracketing samples", firstEvidence(result));
}

void caseWindow(AuditContext& audit, const RuntimePortSampleBuffer& buffer) {
  const auto result = align(buffer, "window", 2.0, 1.0);
  const json value = result.inputs.empty() ? json::object() : result.inputs.front().value;
  const bool ok =
      result.inputs.size() == 1 &&
      result.inputs.front().available &&
      result.inputs.front().status == "window" &&
      result.inputs.front().sample_count == 2 &&
      value.value("representation", "") == "runtime_window_samples" &&
      value.value("sample_count", 0) == 2;
  audit.record("window_returns_temporal_sample_sequence", ok, "window policy returns a bounded sample sequence", firstEvidence(result));
}

void casePredictTo(AuditContext& audit, const RuntimePortSampleBuffer& buffer) {
  const auto result = align(buffer, "predict_to", 1.5, 1.0, "src", 5.0);
  const bool ok =
      result.inputs.size() == 1 &&
      !result.inputs.front().available &&
      result.inputs.front().status == "predict_to_requires_model" &&
      result.hasUnavailableRequiredInputs() &&
      firstEvidence(result).value("strategy", "") == "predict_to" &&
      firstEvidence(result).value("alignment_detail", json::object()).value("prediction_required", false);
  audit.record("predict_to_requires_model_backend", ok, "predict_to is explicit and not silently downgraded to hold", firstEvidence(result));
}

void caseStale(AuditContext& audit, const RuntimePortSampleBuffer& buffer) {
  const auto result = align(buffer, "latest_before", 2.0, 1.0, "stale_src", 0.5);
  const json evidence = firstEvidence(result);
  const bool ok =
      result.inputs.size() == 1 &&
      !result.inputs.front().available &&
      result.inputs.front().status == "stale_input" &&
      result.hasUnavailableRequiredInputs() &&
      evidence.value("stale", false) &&
      evidence.value("temporal_port_contract", json::object()).value("stale", false);
  audit.record("stale_input_is_blocking_evidence", ok, "max_age violation is reported as stale and unavailable", evidence);
}

void caseApplyAlignedInputs(AuditContext& audit, const RuntimePortSampleBuffer& buffer) {
  const auto available = align(buffer, "interpolate", 1.5);
  json upstream = json::object();
  RuntimeInputAlignment::applyAlignedInputs(available, upstream);
  const bool available_ok =
      upstream.contains("input_ports") &&
      upstream.at("input_ports").contains("target.interpolate") &&
      upstream.at("runtime_input_alignment").is_array();

  const auto unavailable = align(buffer, "predict_to", 1.5, 1.0, "src", 5.0);
  RuntimeInputAlignment::applyAlignedInputs(unavailable, upstream);
  const bool unavailable_ok =
      !upstream.at("input_ports").contains("target.predict_to") &&
      upstream.at("runtime_input_alignment").size() == 2;

  audit.record(
      "apply_aligned_inputs_injects_only_available_values",
      available_ok && unavailable_ok,
      "available values are injected; unavailable predict_to evidence is retained only as evidence",
      upstream);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string report_path =
      argc > 1 ? argv[1] : "_local_artifacts/platform-runtime/temporal-input-alignment-audit.json";
  AuditContext audit;
  const RuntimePortSampleBuffer buffer = seedBuffer();

  caseExact(audit, buffer);
  caseLatestBefore(audit, buffer);
  caseHold(audit, buffer);
  caseInterpolate(audit, buffer);
  caseWindow(audit, buffer);
  casePredictTo(audit, buffer);
  caseStale(audit, buffer);
  caseApplyAlignedInputs(audit, buffer);

  const json report = {
      {"schema_version", "flightenv.platform.temporal_input_alignment_phase8_audit.v1"},
      {"status", audit.failures == 0 ? "passed" : "failed"},
      {"summary",
       {
           {"case_count", audit.cases.size()},
           {"failure_count", audit.failures},
           {"covered_strategies", {"exact", "latest_before", "hold", "interpolate", "window", "predict_to"}},
           {"stale_input_gate_covered", true},
       }},
      {"sample_buffer", buffer.evidence()},
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
