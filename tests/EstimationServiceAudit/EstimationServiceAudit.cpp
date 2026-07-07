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
        event.value("checkpoint_id", std::string()).empty() ||
        event.value("posterior_commit_id", std::string()).empty()) {
      throw std::runtime_error("branch event is missing parent/branch/target/checkpoint fields");
    }
    if (!event.value("seed_checkpoint_committed", false) ||
        event.value("source_state_visibility", std::string()) != "committed_only" ||
        event.value("commit_barrier", std::string()) != "posterior_commit") {
      throw std::runtime_error("branch event did not seed from a committed posterior checkpoint");
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

void requireFilterCommitEvidence(const fs::path& out_dir, const json& evidence, int expected_frames) {
  if (evidence.value("execution_mode", std::string()) != "scheduled_predict_update_commit_v1") {
    throw std::runtime_error("estimation evidence did not use scheduled predict/update/commit mode");
  }
  const json summary = evidence.value("summary", json::object());
  if (summary.value("frame_count", -1) != expected_frames ||
      summary.value("committed_frame_count", -1) != expected_frames ||
      summary.value("latest_commit_id", std::string()).empty()) {
    throw std::runtime_error("estimation summary does not match committed frame count");
  }
  const json filter_commit = evidence.value("filter_commit", json::object());
  if (!filter_commit.is_object() ||
      filter_commit.value("phase_model", std::string()) != "predict_update_commit" ||
      filter_commit.value("commit_barrier", std::string()) != "posterior_commit" ||
      filter_commit.value("source_state_visibility", std::string()) != "committed_only") {
    throw std::runtime_error("filter_commit evidence is missing the commit barrier contract");
  }
  const json commit_log = filter_commit.value("commit_log", json::array());
  if (!commit_log.is_array() || static_cast<int>(commit_log.size()) != expected_frames) {
    throw std::runtime_error("posterior commit log count mismatch");
  }
  const json phase_events = filter_commit.value("events", json::array());
  int predict_count = 0;
  int update_count = 0;
  int commit_count = 0;
  for (const auto& event : phase_events) {
    const std::string kind = event.value("runtime_event_kind", event.value("event_kind", std::string()));
    if (kind == "filter_predict") {
      ++predict_count;
    } else if (kind == "filter_update") {
      ++update_count;
    } else if (kind == "posterior_commit") {
      ++commit_count;
      if (!event.value("posterior_committed", false) ||
          event.value("source_state_visibility", std::string()) != "committed_only") {
        throw std::runtime_error("posterior_commit event did not publish a committed state");
      }
    }
  }
  if (predict_count != expected_frames || update_count != expected_frames || commit_count != expected_frames) {
    throw std::runtime_error("filter predict/update/commit phase event count mismatch");
  }

  const json checkpoints = readJson(out_dir / "state_checkpoint.json");
  const json checkpoint_items = checkpoints.value("checkpoints", json::array());
  if (!checkpoint_items.is_array() || static_cast<int>(checkpoint_items.size()) != expected_frames) {
    throw std::runtime_error("checkpoint count mismatch");
  }
  for (const auto& checkpoint : checkpoint_items) {
    if (!checkpoint.value("committed", false) ||
        checkpoint.value("commit_barrier", std::string()) != "posterior_commit" ||
        checkpoint.value("commit_id", std::string()).empty()) {
      throw std::runtime_error("checkpoint is missing committed posterior metadata");
    }
  }

  const json loop = readJson(out_dir / "runtime_loop_summary.json");
  const json iterations = loop.value("iterations", json::array());
  if (!iterations.is_array() || static_cast<int>(iterations.size()) != expected_frames) {
    throw std::runtime_error("runtime_loop_summary iteration count mismatch");
  }
  for (const auto& iteration : iterations) {
    if (!iteration.value("posterior_committed", false) ||
        iteration.value("posterior_commit_id", std::string()).empty()) {
      throw std::runtime_error("runtime loop iteration is missing committed posterior metadata");
    }
  }

  const json scheduler = readJson(out_dir / "scheduler_timeline.json");
  const json scheduler_events = scheduler.value("events", json::array());
  int scheduler_commit_count = 0;
  for (const auto& event : scheduler_events) {
    const std::string kind = event.value("runtime_event_kind", event.value("event_kind", std::string()));
    if (kind == "posterior_commit") {
      ++scheduler_commit_count;
    }
  }
  if (scheduler_commit_count != expected_frames) {
    throw std::runtime_error("scheduler timeline posterior_commit count mismatch");
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
    const auto result = service.runScheduled(request);
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
    requireFilterCommitEvidence(out_dir, evidence, expected_frames);
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
