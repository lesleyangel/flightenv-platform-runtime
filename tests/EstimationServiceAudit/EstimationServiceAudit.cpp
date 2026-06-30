#include "FlightEnvPlatformRuntime/estimation/RuntimeEstimationService.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json readJson(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("JSON not found: " + path.string());
  }
  json value;
  in >> value;
  return value;
}

std::map<std::string, std::string> parseArgs(int argc, char** argv) {
  std::map<std::string, std::string> out;
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i] ? argv[i] : "";
    if (key.rfind("--", 0) != 0) {
      throw std::runtime_error("Unexpected argument: " + key);
    }
    key = key.substr(2);
    if (i + 1 >= argc) {
      throw std::runtime_error("Missing value for --" + key);
    }
    out[key] = argv[++i] ? argv[i] : "";
  }
  return out;
}

std::string requireArg(const std::map<std::string, std::string>& args, const std::string& key) {
  const auto it = args.find(key);
  if (it == args.end() || it->second.empty()) {
    throw std::runtime_error("Missing --" + key);
  }
  return it->second;
}

bool hasMetric(const json& evidence, const std::string& metric) {
  const json frames = evidence.value("frames", json::array());
  if (!frames.is_array() || frames.empty()) {
    return false;
  }
  for (const auto& frame : frames) {
    const json diagnostics = frame.value("diagnostics", json::object());
    if (!diagnostics.is_object() || !diagnostics.contains(metric)) {
      return false;
    }
  }
  return true;
}

std::string expectedSampleKindForMethod(const std::string& method) {
  if (method == "particle_filter") {
    return "particle";
  }
  if (method == "blackbox_unscented_kalman") {
    return "sigma_point";
  }
  if (method == "extended_kalman") {
    return "linearization_state";
  }
  if (method == "ensemble_kalman") {
    return "ensemble_member";
  }
  throw std::runtime_error("Unsupported expected method: " + method);
}

void overrideBatchSize(json& estimation_plan, int batch_size) {
  if (batch_size <= 0) {
    return;
  }
  json& systems = estimation_plan["estimation_systems"];
  if (!systems.is_array() || systems.empty()) {
    throw std::runtime_error("Cannot override batch size without estimation systems");
  }
  json& scheduling = systems[0]["sample_scheduling"];
  if (!scheduling.is_object()) {
    scheduling = json::object();
  }
  scheduling["enabled"] = true;
  scheduling["batch_size"] = batch_size;
}

json readJsonIfExists(const fs::path& path) {
  if (!fs::exists(path)) {
    return json::object();
  }
  return readJson(path);
}

void requireSampleSchedulerEvidence(
    const json& evidence,
    const std::string& method,
    int expected_frames,
    int expected_batch_size) {
  const json scheduler = evidence.value("sample_scheduler", json::object());
  if (!scheduler.is_object() || !scheduler.value("enabled", false)) {
    throw std::runtime_error("sample_scheduler evidence is missing or disabled");
  }
  const json frames = scheduler.value("frames", json::array());
  if (!frames.is_array() || static_cast<int>(frames.size()) != expected_frames) {
    throw std::runtime_error("sample_scheduler frame count mismatch");
  }
  const std::string expected_kind = expectedSampleKindForMethod(method);
  for (const auto& frame : frames) {
    if (frame.value("sample_kind", std::string()) != expected_kind) {
      throw std::runtime_error("unexpected sample_kind in sample scheduler evidence");
    }
    if (expected_batch_size > 0 && frame.value("batch_size", 0) != expected_batch_size) {
      throw std::runtime_error("sample_scheduler batch_size mismatch");
    }
    if (frame.value("total_batch_count", 0) <= 0 ||
        frame.value("accepted_batch_count", 0) != frame.value("total_batch_count", -1) ||
        frame.value("failed_batch_count", -1) != 0) {
      throw std::runtime_error("sample batches were not fully accepted by ReadyQueue");
    }
    const json batches = frame.value("batches", json::array());
    if (!batches.is_array() || batches.empty()) {
      throw std::runtime_error("sample batch details are missing");
    }
    bool saw_transition = false;
    bool saw_observation = false;
    for (const auto& batch : batches) {
      const std::string op = batch.value("operation_kind", std::string());
      saw_transition = saw_transition || op == "state_transition";
      saw_observation = saw_observation || op == "observation_equation";
      const json event = batch.value("event", json::object());
      if (event.value("event_kind", std::string()) != "sample_batch") {
        throw std::runtime_error("sample batch did not emit sample_batch event");
      }
      const json admission = batch.value("ready_queue_admission", json::object());
      if (!admission.value("accepted", false)) {
        throw std::runtime_error("sample batch did not enter ReadyQueue");
      }
      if (!batch.contains("typed_input_ref") || !batch.contains("typed_output_ref")) {
        throw std::runtime_error("sample batch is missing typed buffer refs");
      }
    }
    if (!saw_transition || !saw_observation) {
      throw std::runtime_error("sample scheduler did not cover both transition and observation batches");
    }
  }
}

void requireBranchEvidence(const fs::path& out_dir, const json& evidence, int expected_branches) {
  if (expected_branches < 0) {
    return;
  }
  const json branches = evidence.value("branches", json::object());
  if (!branches.is_object()) {
    throw std::runtime_error("branch evidence is missing");
  }
  const json events = branches.value("events", json::array());
  if (!events.is_array() || static_cast<int>(events.size()) != expected_branches) {
    throw std::runtime_error("branch trigger count mismatch");
  }
  for (const auto& event : events) {
    if (event.value("event_kind", std::string()) != "branch_triggered") {
      throw std::runtime_error("unexpected branch event kind");
    }
    if (event.value("parent_branch_id", std::string()).empty() ||
        event.value("branch_id", std::string()).empty() ||
        event.value("target_workflow_id", std::string()).empty() ||
        event.value("checkpoint_id", std::string()).empty()) {
      throw std::runtime_error("branch event is missing parent/branch/target/checkpoint fields");
    }
    if (event.value("status", std::string()) != "ready_to_start") {
      throw std::runtime_error("branch event is not ready_to_start");
    }
  }
  const json timeline = readJson(out_dir / "branch_timeline.json");
  const json timeline_events = timeline.value("events", json::array());
  if (!timeline_events.is_array() || static_cast<int>(timeline_events.size()) != expected_branches) {
    throw std::runtime_error("branch_timeline event count mismatch");
  }
}

void requireMetricSet(const json& evidence, const std::string& method) {
  if (method == "particle_filter") {
    for (const std::string& metric : {"particle_count", "ess", "resampling_count", "posterior_checkpoint"}) {
      if (!hasMetric(evidence, metric)) {
        throw std::runtime_error("PF evidence missing metric: " + metric);
      }
    }
    return;
  }
  if (method == "blackbox_unscented_kalman") {
    for (const std::string& metric :
         {"sigma_point_count", "innovation_norm", "posterior_cov_trace", "jitter_count", "posterior_checkpoint"}) {
      if (!hasMetric(evidence, metric)) {
        throw std::runtime_error("UKF evidence missing metric: " + metric);
      }
    }
    const json frames = evidence.value("frames", json::array());
    for (const auto& frame : frames) {
      const json diagnostics = frame.value("diagnostics", json::object());
      if (!diagnostics.value("covariance_symmetric", false) ||
          !diagnostics.value("covariance_diag_nonnegative", false)) {
        throw std::runtime_error("UKF covariance diagnostic failed");
      }
    }
    return;
  }
  if (method == "extended_kalman") {
    for (const std::string& metric :
         {"linearization", "jacobian_provider", "innovation_norm", "posterior_cov_trace", "posterior_checkpoint"}) {
      if (!hasMetric(evidence, metric)) {
        throw std::runtime_error("EKF evidence missing metric: " + metric);
      }
    }
    const json frames = evidence.value("frames", json::array());
    for (const auto& frame : frames) {
      const json diagnostics = frame.value("diagnostics", json::object());
      if (!diagnostics.value("requires_jacobian", false) ||
          !diagnostics.value("covariance_diag_nonnegative", false)) {
        throw std::runtime_error("EKF capability or covariance diagnostic failed");
      }
    }
    return;
  }
  if (method == "ensemble_kalman") {
    for (const std::string& metric :
         {"ensemble_size", "inflation_factor", "ensemble_spread_trace", "posterior_cov_trace", "posterior_checkpoint"}) {
      if (!hasMetric(evidence, metric)) {
        throw std::runtime_error("EnKF evidence missing metric: " + metric);
      }
    }
    const json frames = evidence.value("frames", json::array());
    for (const auto& frame : frames) {
      const json diagnostics = frame.value("diagnostics", json::object());
      if (!diagnostics.value("covariance_diag_nonnegative", false)) {
        throw std::runtime_error("EnKF covariance diagnostic failed");
      }
    }
    return;
  }
  throw std::runtime_error("Unsupported expected method: " + method);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto args = parseArgs(argc, argv);
    const fs::path estimation_plan_path = requireArg(args, "estimation-plan");
    const fs::path observation_stream_path = requireArg(args, "observation-stream");
    const fs::path out_dir = requireArg(args, "out-dir");
    const std::string expected_method = requireArg(args, "expect-method");
    const int expected_frames = std::stoi(requireArg(args, "expect-frames"));
    const bool require_sample_scheduler = args.find("require-sample-scheduler") != args.end() &&
                                          args.at("require-sample-scheduler") == "true";
    const int batch_size_override = args.find("batch-size") == args.end()
                                        ? 0
                                        : std::stoi(args.at("batch-size"));
    const int expected_branches = args.find("expect-branches") == args.end()
                                      ? -1
                                      : std::stoi(args.at("expect-branches"));

    FlightEnvPlatformRuntime::estimation::RuntimeEstimationRequest request;
    request.run_dir = out_dir;
    request.compiled_workflow_dir = estimation_plan_path.parent_path();
    request.run_id = "estimation_service_audit_" + expected_method;
    request.estimation_plan = readJson(estimation_plan_path);
    overrideBatchSize(request.estimation_plan, batch_size_override);
    request.scheduler_plan = readJsonIfExists(estimation_plan_path.parent_path() / "scheduler_plan.json");
    request.workflow_snapshot = readJsonIfExists(estimation_plan_path.parent_path() / "workflow_snapshot.json");
    request.external_observations = readJson(observation_stream_path);
    request.workflow_id = request.estimation_plan.value("workflow_id", std::string("workflow.audit"));
    request.object_id = request.estimation_plan.value("object_id", std::string("object.audit"));
    request.branch_id = "audit.main";
    request.timeline_id = "audit.timeline";
    request.max_frames = expected_frames;

    FlightEnvPlatformRuntime::estimation::RuntimeEstimationService service;
    const auto result = service.runSerial(request);
    if (result.exit_code != 0) {
      throw std::runtime_error("service returned non-zero exit code");
    }
    if (result.frame_count != expected_frames) {
      throw std::runtime_error("unexpected frame count: " + std::to_string(result.frame_count));
    }
    const json evidence = readJson(out_dir / "estimation_evidence.json");
    if (evidence.value("method", std::string()) != expected_method) {
      throw std::runtime_error("unexpected method in evidence");
    }
    requireMetricSet(evidence, expected_method);
    if (require_sample_scheduler) {
      requireSampleSchedulerEvidence(evidence, expected_method, expected_frames, batch_size_override);
    }
    requireBranchEvidence(out_dir, evidence, expected_branches);
    std::cout << "[PASS] EstimationServiceAudit " << expected_method
              << " frames=" << result.frame_count
              << " out=" << out_dir.string() << "\n";
    return 0;
  } catch (const std::exception& exc) {
    std::cerr << "[FAIL] " << exc.what() << "\n";
    return 2;
  }
}
