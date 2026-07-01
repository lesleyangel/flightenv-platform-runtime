#include "FlightEnvPlatformRuntime/estimation/RuntimeEstimationService.hpp"

#include <cmath>
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

json readJsonIfExists(const fs::path& path) {
  if (!fs::exists(path)) {
    return json::object();
  }
  return readJson(path);
}

void writeJson(const fs::path& path, const json& value) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("Cannot write JSON: " + path.string());
  }
  out << value.dump(2) << '\n';
}

std::map<std::string, std::string> parseArgs(int argc, char** argv) {
  std::map<std::string, std::string> out;
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i] ? argv[i] : "";
    if (key.rfind("--", 0) != 0 || i + 1 >= argc) {
      throw std::runtime_error("Expected --key value arguments");
    }
    key = key.substr(2);
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

json makeObservationFrame(
    int frame_index,
    double sample_time_s,
    double arrival_time_s,
    double base_value) {
  return {
      {"frame_index", frame_index},
      {"sample_time_s", sample_time_s},
      {"arrival_time_s", arrival_time_s},
      {"sensor_count", 3},
      {"source", "phase10_closed_loop_audit"},
      {"selected_state",
       {
           {"state_0", base_value},
           {"state_1", base_value * 0.5 + 1.0},
           {"state_2", base_value * 0.25 - 0.5},
       }},
  };
}

json makeMissingFrame(int frame_index, double sample_time_s) {
  return {
      {"frame_index", frame_index},
      {"sample_time_s", sample_time_s},
      {"arrival_time_s", sample_time_s},
      {"sensor_count", 0},
      {"source", "phase10_closed_loop_audit"},
      {"observation_status", "missing"},
      {"missing_observation", true},
  };
}

json makeStream(const json& frames) {
  return {
      {"schema_version", "flightenv.platform.phase10.audit_observation_stream.v1"},
      {"frames", frames},
  };
}

json mutatePlanForAudit(json plan) {
  json& systems = plan["estimation_systems"];
  if (!systems.is_array() || systems.empty()) {
    throw std::runtime_error("estimation_plan has no estimation_systems");
  }
  json& method = systems[0]["method"];
  method["particle_count"] = 64;
  method["random_seed_policy"] = "fixed";
  method["seed"] = 1001;
  json& scheduling = systems[0]["sample_scheduling"];
  scheduling["enabled"] = true;
  scheduling["batch_size"] = 16;
  scheduling["max_parallel_batches"] = 2;
  scheduling["typed_buffer_mode"] = "memory_only";
  json& diagnostics = systems[0]["diagnostics_policy"];
  diagnostics["late_observation_tolerance_s"] = 0.05;
  return plan;
}

json workflowSnapshotForAudit(json snapshot) {
  if (!snapshot.is_object()) {
    snapshot = json::object();
  }
  if (!snapshot.contains("workflow") || !snapshot["workflow"].is_object()) {
    snapshot["workflow"] = json::object();
  }
  snapshot["workflow"]["branching_policy"] = {
      {"enabled", true},
      {"target_workflow_id", "phase10.forecast.audit.v1"},
      {"trigger_kind", "every_n_frames"},
      {"every_n_frames", 3},
      {"seed_policy", "latest_checkpoint"},
      {"max_concurrent_branches", 8},
  };
  return snapshot;
}

json runService(
    const fs::path& out_dir,
    const fs::path& compiled_dir,
    const json& estimation_plan,
    const json& scheduler_plan,
    const json& workflow_snapshot,
    const json& stream,
    const json& resume_checkpoint = json::object()) {
  fs::remove_all(out_dir);
  fs::create_directories(out_dir);
  FlightEnvPlatformRuntime::estimation::RuntimeEstimationRequest request;
  request.run_dir = out_dir;
  request.compiled_workflow_dir = compiled_dir;
  request.run_id = "phase10_closed_loop_audit";
  request.workflow_id = estimation_plan.value("workflow_id", std::string("workflow.audit"));
  request.object_id = estimation_plan.value("object_id", std::string("object.audit"));
  request.branch_id = "audit.main";
  request.timeline_id = "audit.timeline";
  request.estimation_plan = estimation_plan;
  request.scheduler_plan = scheduler_plan;
  request.workflow_snapshot = workflow_snapshot;
  request.external_observations = stream;
  request.resume_state_checkpoint = resume_checkpoint;
  request.max_frames = static_cast<int>(stream.value("frames", json::array()).size());
  FlightEnvPlatformRuntime::estimation::RuntimeEstimationService service;
  const auto result = service.runScheduled(request);
  if (result.exit_code != 0) {
    throw std::runtime_error("RuntimeEstimationService failed");
  }
  return readJson(out_dir / "estimation_evidence.json");
}

const json& requireObject(const json& value, const std::string& key) {
  if (!value.contains(key) || !value.at(key).is_object()) {
    throw std::runtime_error("Missing object: " + key);
  }
  return value.at(key);
}

const json& requireArray(const json& value, const std::string& key) {
  if (!value.contains(key) || !value.at(key).is_array()) {
    throw std::runtime_error("Missing array: " + key);
  }
  return value.at(key);
}

const json* frameByIndex(const json& frames, int frame_index) {
  for (const auto& frame : frames) {
    if (frame.value("frame_index", -1) == frame_index) {
      return &frame;
    }
  }
  return nullptr;
}

void requireNearArrays(const json& a, const json& b, const std::string& label) {
  if (!a.is_array() || !b.is_array() || a.size() != b.size()) {
    throw std::runtime_error(label + " array shape mismatch");
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double da = a.at(i).get<double>();
    const double db = b.at(i).get<double>();
    if (std::abs(da - db) > 1.0e-9) {
      throw std::runtime_error(label + " mismatch at " + std::to_string(i));
    }
  }
}

void requireMissingLateEvidence(const fs::path& run_dir, const json& evidence) {
  const json summary = requireObject(evidence, "summary");
  if (summary.value("missing_observation_count", -1) != 1 ||
      summary.value("late_observation_count", -1) != 1 ||
      summary.value("predict_only_frame_count", -1) != 1 ||
      summary.value("update_frame_count", -1) != 3) {
    throw std::runtime_error("missing/late/predict-only summary mismatch");
  }
  const json timing = requireObject(evidence, "observation_timing");
  const json timing_events = requireArray(timing, "events");
  bool saw_missing = false;
  bool saw_late = false;
  for (const auto& event : timing_events) {
    saw_missing = saw_missing || event.value("missing_observation", false);
    saw_late = saw_late || event.value("late_observation", false);
  }
  if (!saw_missing || !saw_late) {
    throw std::runtime_error("missing or late observation timing event not found");
  }
  const json filter_commit = requireObject(evidence, "filter_commit");
  const json phase_events = requireArray(filter_commit, "events");
  bool saw_skipped_update = false;
  for (const auto& event : phase_events) {
    if (event.value("runtime_event_kind", std::string()) == "filter_update" &&
        event.value("status", std::string()) == "skipped" &&
        event.value("reason", std::string()) == "missing_observation") {
      saw_skipped_update = true;
    }
  }
  if (!saw_skipped_update) {
    throw std::runtime_error("predict-only missing observation update skip was not recorded");
  }
  const json checkpoint = readJson(run_dir / "state_checkpoint.json");
  for (const auto& item : requireArray(checkpoint, "checkpoints")) {
    if (!item.value("committed", false) ||
        !item.contains("estimator_state") ||
        !item.at("estimator_state").is_object()) {
      throw std::runtime_error("checkpoint does not carry committed estimator state");
    }
  }
}

void requireForecastReadOnlyEvidence(const json& evidence) {
  const json branches = requireObject(evidence, "branches");
  const json events = requireArray(branches, "events");
  if (events.empty()) {
    throw std::runtime_error("forecast branch event was not triggered");
  }
  for (const auto& event : events) {
    if (!event.value("seed_checkpoint_committed", false) ||
        event.value("source_state_visibility", std::string()) != "committed_only" ||
        event.value("forecast_state_access", std::string()) != "committed_checkpoint_read_only" ||
        event.value("forecast_mutates_online_posterior", true)) {
      throw std::runtime_error("forecast branch did not use committed read-only posterior state");
    }
  }
}

json singleCheckpointAtFrame(const fs::path& run_dir, int frame_index) {
  const json checkpoint = readJson(run_dir / "state_checkpoint.json");
  json selected = json::array();
  for (const auto& item : requireArray(checkpoint, "checkpoints")) {
    if (item.value("frame_index", -1) == frame_index) {
      selected.push_back(item);
    }
  }
  if (selected.empty()) {
    throw std::runtime_error("checkpoint not found for frame " + std::to_string(frame_index));
  }
  return {
      {"schema_version", "flightenv.platform.state_checkpoint.v1"},
      {"checkpoints", selected},
  };
}

void requireResumeMatchesFull(const json& full_evidence, const json& resume_evidence, int first_resumed_frame) {
  const json resume = requireObject(resume_evidence, "checkpoint_resume");
  if (!resume.value("resumed_from_checkpoint", false) ||
      !resume.value("estimator_state_restored", false)) {
    throw std::runtime_error("resume checkpoint was not restored with estimator state");
  }
  const json full_frames = requireArray(full_evidence, "frames");
  const json resumed_frames = requireArray(resume_evidence, "frames");
  for (const auto& resumed : resumed_frames) {
    const int frame_index = resumed.value("frame_index", -1);
    if (frame_index < first_resumed_frame) {
      throw std::runtime_error("unexpected resumed frame index");
    }
    const json* full = frameByIndex(full_frames, frame_index);
    if (full == nullptr) {
      throw std::runtime_error("full-run frame missing for resumed frame");
    }
    requireNearArrays(full->at("state_mean"), resumed.at("state_mean"), "state_mean");
    requireNearArrays(full->at("covariance_diag"), resumed.at("covariance_diag"), "covariance_diag");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto args = parseArgs(argc, argv);
    const fs::path estimation_plan_path = requireArg(args, "estimation-plan");
    const fs::path out_root = requireArg(args, "out-root");
    const fs::path compiled_dir = estimation_plan_path.parent_path();
    const json estimation_plan = mutatePlanForAudit(readJson(estimation_plan_path));
    const json scheduler_plan = readJsonIfExists(compiled_dir / "scheduler_plan.json");
    const json workflow_snapshot = workflowSnapshotForAudit(readJsonIfExists(compiled_dir / "workflow_snapshot.json"));

    json full_frames = json::array();
    for (int i = 0; i < 8; ++i) {
      full_frames.push_back(makeObservationFrame(i, 0.2 * static_cast<double>(i), 0.2 * static_cast<double>(i), 10.0 + i));
    }
    const json full_stream = makeStream(full_frames);
    const fs::path full_dir = out_root / "full";
    const json full_evidence =
        runService(full_dir, compiled_dir, estimation_plan, scheduler_plan, workflow_snapshot, full_stream);
    requireForecastReadOnlyEvidence(full_evidence);

    json missing_late_frames = json::array();
    missing_late_frames.push_back(makeObservationFrame(0, 0.00, 0.00, 1.0));
    missing_late_frames.push_back(makeMissingFrame(1, 0.10));
    missing_late_frames.push_back(makeObservationFrame(2, 0.40, 0.62, 2.0));
    missing_late_frames.push_back(makeObservationFrame(3, 0.45, 0.45, 3.0));
    const fs::path missing_late_dir = out_root / "missing_late";
    const json missing_late_evidence = runService(
        missing_late_dir,
        compiled_dir,
        estimation_plan,
        scheduler_plan,
        workflow_snapshot,
        makeStream(missing_late_frames));
    requireMissingLateEvidence(missing_late_dir, missing_late_evidence);

    json resumed_frames = json::array();
    for (int i = 4; i < 8; ++i) {
      resumed_frames.push_back(makeObservationFrame(i, 0.2 * static_cast<double>(i), 0.2 * static_cast<double>(i), 10.0 + i));
    }
    const json resume_checkpoint = singleCheckpointAtFrame(full_dir, 3);
    const fs::path resume_dir = out_root / "resume_from_frame_3";
    const json resume_evidence = runService(
        resume_dir,
        compiled_dir,
        estimation_plan,
        scheduler_plan,
        workflow_snapshot,
        makeStream(resumed_frames),
        resume_checkpoint);
    requireResumeMatchesFull(full_evidence, resume_evidence, 4);

    const json report = {
        {"schema_version", "flightenv.platform.phase10_estimation_closed_loop_audit.v1"},
        {"full_run", full_dir.string()},
        {"missing_late_run", missing_late_dir.string()},
        {"resume_run", resume_dir.string()},
        {"full_committed_frame_count", requireObject(full_evidence, "summary").value("committed_frame_count", 0)},
        {"missing_observation_count", requireObject(missing_late_evidence, "summary").value("missing_observation_count", 0)},
        {"late_observation_count", requireObject(missing_late_evidence, "summary").value("late_observation_count", 0)},
        {"predict_only_frame_count", requireObject(missing_late_evidence, "summary").value("predict_only_frame_count", 0)},
        {"resume_matches_full_from_frame", 4},
        {"forecast_uses_committed_read_only_checkpoint", true},
    };
    writeJson(out_root / "phase10_estimation_closed_loop_audit.json", report);
    std::cout << "[PASS] EstimationClosedLoopAudit out=" << out_root.string() << "\n";
    return 0;
  } catch (const std::exception& exc) {
    std::cerr << "[FAIL] " << exc.what() << "\n";
    return 2;
  }
}
