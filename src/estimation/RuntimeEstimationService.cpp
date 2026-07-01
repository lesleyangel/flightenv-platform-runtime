#include "FlightEnvPlatformRuntime/estimation/RuntimeEstimationService.hpp"

#include "FlightEnvPlatformRuntime/RuntimeClock.hpp"
#include "FlightEnvPlatformRuntime/RuntimeEvidenceWriter.hpp"
#include "FlightEnvPlatformRuntime/estimation/EstimatorMethodRegistry.hpp"
#include "FlightEnvPlatformRuntime/estimation/RuntimeObservationInbox.hpp"
#include "FlightEnvPlatformRuntime/estimation/SampleScheduler.hpp"
#include "FlightEnvPlatformRuntime/estimation/SampleSetStore.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace FlightEnvPlatformRuntime::estimation {
namespace {

std::string jsonString(const nlohmann::json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

nlohmann::json vectorJson(const std::vector<double>& values) {
  nlohmann::json out = nlohmann::json::array();
  for (double value : values) {
    out.push_back(std::isfinite(value) ? value : 0.0);
  }
  return out;
}

nlohmann::json labelsJson(const std::vector<std::string>& labels) {
  nlohmann::json out = nlohmann::json::array();
  for (const std::string& label : labels) {
    out.push_back(label);
  }
  return out;
}

nlohmann::json posteriorPortPayload(const PosteriorFrame& frame) {
  return {
      {"frame_contract", "StateSnapshot.v1"},
      {"contract_id", "platform.state_snapshot.v1"},
      {"representation", "estimation_posterior_vector"},
      {"frame_index", frame.frame_index},
      {"sample_time_s", frame.sample_time_s},
      {"labels", labelsJson(frame.value_labels)},
      {"values", vectorJson(frame.state_mean)},
      {"checkpoint_id", frame.checkpoint_id},
      {"commit_id", frame.commit_id},
      {"committed", frame.committed},
      {"valid_time_s", frame.sample_time_s},
      {"commit_barrier", frame.committed ? "posterior_commit" : "uncommitted"},
  };
}

nlohmann::json uncertaintyPortPayload(const PosteriorFrame& frame) {
  return {
      {"frame_contract", "StateSnapshot.v1"},
      {"contract_id", "platform.uncertainty_summary.v1"},
      {"representation", "diagonal_covariance"},
      {"frame_index", frame.frame_index},
      {"sample_time_s", frame.sample_time_s},
      {"labels", labelsJson(frame.value_labels)},
      {"covariance_diag", vectorJson(frame.covariance_diag)},
      {"commit_id", frame.commit_id},
      {"committed", frame.committed},
      {"valid_time_s", frame.sample_time_s},
  };
}

nlohmann::json frameEvidence(const PosteriorFrame& frame) {
  return {
      {"frame_index", frame.frame_index},
      {"sample_time_s", frame.sample_time_s},
      {"posterior_checkpoint", frame.checkpoint_id},
      {"posterior_commit_id", frame.commit_id},
      {"posterior_committed", frame.committed},
      {"commit_barrier", frame.committed ? "posterior_commit" : "uncommitted"},
      {"state_mean", vectorJson(frame.state_mean)},
      {"covariance_diag", vectorJson(frame.covariance_diag)},
      {"diagnostics", frame.diagnostics},
  };
}

std::string sampleKindForMethod(const std::string& method_kind) {
  if (method_kind == "particle_filter") {
    return "particle";
  }
  if (method_kind == "blackbox_unscented_kalman") {
    return "sigma_point";
  }
  if (method_kind == "extended_kalman") {
    return "linearization_state";
  }
  if (method_kind == "ensemble_kalman") {
    return "ensemble_member";
  }
  return "ensemble_member";
}

int sampleCountForMethod(const std::string& method_kind, const nlohmann::json& method_config, std::size_t state_dim) {
  if (method_kind == "particle_filter") {
    return std::max(1, method_config.value("particle_count", 1));
  }
  if (method_kind == "blackbox_unscented_kalman") {
    return static_cast<int>(2 * state_dim + 1);
  }
  if (method_kind == "extended_kalman") {
    return static_cast<int>(std::max<std::size_t>(1, state_dim + 1));
  }
  if (method_kind == "ensemble_kalman") {
    return std::max(1, method_config.value("ensemble_size", 1));
  }
  return static_cast<int>(std::max<std::size_t>(1, state_dim));
}

nlohmann::json compactSampleScheduleDiagnostics(const nlohmann::json& schedule) {
  return {
      {"enabled", schedule.value("enabled", false)},
      {"sample_kind", schedule.value("sample_kind", std::string())},
      {"sample_count", schedule.value("sample_count", 0)},
      {"batch_size", schedule.value("batch_size", 0)},
      {"total_batch_count", schedule.value("total_batch_count", 0)},
      {"accepted_batch_count", schedule.value("accepted_batch_count", 0)},
      {"failed_batch_count", schedule.value("failed_batch_count", 0)},
  };
}

nlohmann::json workflowFromSnapshot(const nlohmann::json& snapshot) {
  if (!snapshot.is_object()) {
    return nlohmann::json::object();
  }
  const nlohmann::json workflow = snapshot.value("workflow", nlohmann::json::object());
  return workflow.is_object() ? workflow : nlohmann::json::object();
}

int jsonInt(const nlohmann::json& value, const std::string& key, int fallback = 0) {
  if (!value.is_object() || !value.contains(key)) {
    return fallback;
  }
  const nlohmann::json& item = value.at(key);
  if (item.is_number_integer()) {
    return item.get<int>();
  }
  if (item.is_number()) {
    return static_cast<int>(item.get<double>());
  }
  return fallback;
}

bool shouldTriggerBranch(
    const nlohmann::json& policy,
    int processed_frame_count,
    int triggered_branch_count) {
  if (!policy.is_object() || !policy.value("enabled", false)) {
    return false;
  }
  if (policy.value("trigger_kind", std::string()) != "every_n_frames") {
    return false;
  }
  const int every_n_frames = jsonInt(policy, "every_n_frames", 0);
  if (every_n_frames <= 0 || processed_frame_count <= 0 ||
      processed_frame_count % every_n_frames != 0) {
    return false;
  }
  const int max_branches = jsonInt(policy, "max_concurrent_branches", 0);
  return max_branches <= 0 || triggered_branch_count < max_branches;
}

std::string posteriorCommitId(const RuntimeEstimationRequest& request, int frame_index) {
  std::string prefix = request.branch_id.empty() ? std::string("main") : request.branch_id;
  std::replace_if(prefix.begin(), prefix.end(), [](unsigned char c) {
    return !(std::isalnum(c) || c == '_' || c == '-' || c == '.');
  }, '_');
  return "posterior_commit." + prefix + ".frame_" + std::to_string(frame_index);
}

nlohmann::json filterPhaseEvent(
    const std::string& phase,
    const std::string& status,
    const std::string& stage_id,
    const RuntimeObservationFrame& observation,
    const std::string& branch_id,
    const std::string& timeline_id,
    const std::string& previous_checkpoint,
    const std::string& posterior_checkpoint,
    const std::string& commit_id) {
  nlohmann::json event = {
      {"timestamp_utc", RuntimeClock::nowUtcIso()},
      {"event", phase},
      {"event_kind", phase},
      {"runtime_event_kind", phase},
      {"filter_phase", phase},
      {"status", status},
      {"stage_id", stage_id},
      {"branch_id", branch_id},
      {"timeline_id", timeline_id},
      {"frame_index", observation.frame_index},
      {"sample_time_s", observation.sample_time_s},
  };
  if (!previous_checkpoint.empty()) {
    event["previous_checkpoint"] = previous_checkpoint;
  }
  if (!posterior_checkpoint.empty()) {
    event["posterior_checkpoint"] = posterior_checkpoint;
  }
  if (!commit_id.empty()) {
    event["posterior_commit_id"] = commit_id;
  }
  if (phase == "posterior_commit") {
    event["posterior_committed"] = status == "committed";
    event["commit_barrier"] = "posterior_commit";
    event["source_state_visibility"] = "committed_only";
  }
  return event;
}

nlohmann::json branchTriggerEvent(
    const RuntimeEstimationRequest& request,
    const nlohmann::json& policy,
    const PosteriorFrame& frame,
    int branch_index) {
  const std::string target_workflow_id = policy.value("target_workflow_id", std::string());
  const std::string branch_id =
      request.branch_id + ".future_" + std::to_string(branch_index + 1);
  return {
      {"event", "branch_triggered"},
      {"event_kind", "branch_triggered"},
      {"timestamp_utc", RuntimeClock::nowUtcIso()},
      {"parent_branch_id", request.branch_id},
      {"branch_id", branch_id},
      {"target_workflow_id", target_workflow_id},
      {"trigger_frame_index", frame.frame_index},
      {"trigger_time_s", frame.sample_time_s},
      {"checkpoint_id", frame.checkpoint_id},
      {"posterior_commit_id", frame.commit_id},
      {"seed_checkpoint_committed", frame.committed},
      {"source_state_visibility", "committed_only"},
      {"commit_barrier", "posterior_commit"},
      {"seed_policy", policy.value("seed_policy", std::string("latest_checkpoint"))},
      {"cause_event_id", frame.commit_id.empty() ? "posterior_commit." + std::to_string(frame.frame_index) : frame.commit_id},
      {"status", target_workflow_id.empty() ? "declared_without_target" : "ready_to_start"},
  };
}

RuntimeEstimationResult runEstimation(const RuntimeEstimationRequest& request, const std::string& execution_mode) {
  const nlohmann::json systems = request.estimation_plan.value("estimation_systems", nlohmann::json::array());
  if (!systems.is_array() || systems.empty()) {
    throw std::runtime_error("RuntimeEstimationService requires estimation_plan.estimation_systems");
  }
  RuntimeObservationInbox inbox = RuntimeObservationInbox::fromJsonStream(
      request.external_observations,
      request.max_frames);
  if (inbox.empty()) {
    throw std::runtime_error("RuntimeEstimationService requires at least one numeric observation frame");
  }

  const nlohmann::json& system = systems.at(0);
  const nlohmann::json method_config = system.value("method", nlohmann::json::object());
  const std::string method_kind = jsonString(method_config, "kind");
  if (method_kind.empty()) {
    throw std::runtime_error("estimation method.kind is missing");
  }
  const std::size_t state_dim = std::max<std::size_t>(1, inbox.frames().front().values.size());
  std::unique_ptr<IEstimatorMethod> method = EstimatorMethodRegistry::create(method_kind);
  method->configure(method_config, state_dim);

  SampleSetStore store;
  SampleScheduler sample_scheduler;
  nlohmann::json frame_evidence = nlohmann::json::array();
  nlohmann::json sample_scheduler_frames = nlohmann::json::array();
  nlohmann::json scheduler_events = nlohmann::json::array();
  nlohmann::json loop_iterations = nlohmann::json::array();
  nlohmann::json branch_events = nlohmann::json::array();
  nlohmann::json filter_phase_events = nlohmann::json::array();
  nlohmann::json commit_log = nlohmann::json::array();
  const nlohmann::json workflow = workflowFromSnapshot(request.workflow_snapshot);
  const nlohmann::json branching_policy = workflow.value("branching_policy", nlohmann::json::object());
  const std::string stage_id = jsonString(system, "compiled_stage_id", "online_current.online_estimation");
  scheduler_events.push_back({
      {"timestamp_utc", RuntimeClock::nowUtcIso()},
      {"event", "estimation_session_begin"},
      {"stage_id", stage_id},
      {"estimation_system_id", jsonString(system, "estimation_system_id")},
      {"method", method_kind},
      {"frame_count", inbox.size()},
      {"execution_mode", execution_mode},
  });

  for (const RuntimeObservationFrame& observation : inbox.frames()) {
    nlohmann::json frame_phase_events = nlohmann::json::array();
    scheduler_events.push_back({
        {"timestamp_utc", RuntimeClock::nowUtcIso()},
        {"event", "estimation_frame_begin"},
        {"stage_id", stage_id},
        {"frame_index", observation.frame_index},
        {"sample_time_s", observation.sample_time_s},
    });
    const PosteriorFrame* previous_frame = store.latest();
    const std::string previous_checkpoint =
        previous_frame == nullptr ? std::string() : previous_frame->checkpoint_id;
    RuntimeSampleSchedulingRequest sample_request;
    sample_request.run_dir = request.run_dir;
    sample_request.estimation_system = system;
    sample_request.scheduler_plan = request.scheduler_plan;
    sample_request.stage_id = stage_id;
    sample_request.method_kind = method_kind;
    sample_request.sample_kind = sampleKindForMethod(method_kind);
    sample_request.branch_id = request.branch_id;
    sample_request.timeline_id = request.timeline_id;
    sample_request.frame_index = observation.frame_index;
    sample_request.sample_time_s = observation.sample_time_s;
    sample_request.sample_count = sampleCountForMethod(method_kind, method_config, state_dim);
    sample_request.state_dim = static_cast<int>(state_dim);
    nlohmann::json sample_schedule = sample_scheduler.scheduleFrame(sample_request);
    sample_scheduler_frames.push_back(sample_schedule);
    if (sample_schedule.contains("batches") && sample_schedule.at("batches").is_array()) {
      for (const auto& batch : sample_schedule.at("batches")) {
        scheduler_events.push_back({
            {"timestamp_utc", RuntimeClock::nowUtcIso()},
            {"event", "sample_batch"},
            {"stage_id", stage_id},
            {"frame_index", observation.frame_index},
            {"sample_time_s", observation.sample_time_s},
            {"batch", batch},
        });
      }
    }
    nlohmann::json predict_event = filterPhaseEvent(
        "filter_predict",
        "completed",
        stage_id,
        observation,
        request.branch_id,
        request.timeline_id,
        previous_checkpoint,
        "",
        "");
    predict_event["sample_scheduler"] = compactSampleScheduleDiagnostics(sample_schedule);
    scheduler_events.push_back(predict_event);
    filter_phase_events.push_back(predict_event);
    frame_phase_events.push_back(predict_event);
    EstimatorStepRequest step_request;
    step_request.observation = observation;
    step_request.previous = previous_frame;
    EstimatorStepResult step_result = method->step(step_request);
    step_result.posterior.commit_id = posteriorCommitId(request, observation.frame_index);
    step_result.posterior.committed = false;
    step_result.posterior.diagnostics["sample_scheduler"] =
        compactSampleScheduleDiagnostics(sample_schedule);
    step_result.posterior.diagnostics["posterior_commit_id"] = step_result.posterior.commit_id;
    step_result.posterior.diagnostics["posterior_committed"] = true;
    step_result.posterior.diagnostics["filter_execution_mode"] = execution_mode;
    nlohmann::json update_event = filterPhaseEvent(
        "filter_update",
        "completed",
        stage_id,
        observation,
        request.branch_id,
        request.timeline_id,
        previous_checkpoint,
        step_result.posterior.checkpoint_id,
        "");
    update_event["diagnostics"] = step_result.posterior.diagnostics;
    scheduler_events.push_back(update_event);
    filter_phase_events.push_back(update_event);
    frame_phase_events.push_back(update_event);
    step_result.posterior.committed = true;
    store.append(step_result.posterior);
    nlohmann::json commit_event = filterPhaseEvent(
        "posterior_commit",
        "committed",
        stage_id,
        observation,
        request.branch_id,
        request.timeline_id,
        previous_checkpoint,
        step_result.posterior.checkpoint_id,
        step_result.posterior.commit_id);
    commit_event["checkpoint_id"] = step_result.posterior.checkpoint_id;
    commit_event["committed"] = true;
    scheduler_events.push_back(commit_event);
    filter_phase_events.push_back(commit_event);
    frame_phase_events.push_back(commit_event);
    commit_log.push_back({
        {"frame_index", observation.frame_index},
        {"sample_time_s", observation.sample_time_s},
        {"posterior_checkpoint", step_result.posterior.checkpoint_id},
        {"posterior_commit_id", step_result.posterior.commit_id},
        {"posterior_committed", true},
        {"commit_barrier", "posterior_commit"},
    });
    frame_evidence.push_back(frameEvidence(step_result.posterior));
    const int processed_frame_count = static_cast<int>(store.frames().size());
    if (shouldTriggerBranch(branching_policy, processed_frame_count, static_cast<int>(branch_events.size()))) {
      const nlohmann::json branch_event =
          branchTriggerEvent(request, branching_policy, step_result.posterior, static_cast<int>(branch_events.size()));
      branch_events.push_back(branch_event);
      scheduler_events.push_back(branch_event);
    }
    loop_iterations.push_back({
        {"loop_iteration_index", observation.frame_index},
        {"frame_index", observation.frame_index},
        {"sample_time_s", observation.sample_time_s},
        {"status", "ok"},
        {"posterior_checkpoint", step_result.posterior.checkpoint_id},
        {"posterior_commit_id", step_result.posterior.commit_id},
        {"posterior_committed", step_result.posterior.committed},
        {"commit_barrier", "posterior_commit"},
        {"filter_phase_events", frame_phase_events},
        {"diagnostics", step_result.posterior.diagnostics},
        {"sample_scheduler", compactSampleScheduleDiagnostics(sample_schedule)},
    });
    scheduler_events.push_back({
        {"timestamp_utc", RuntimeClock::nowUtcIso()},
        {"event", "estimation_frame_finish"},
        {"stage_id", stage_id},
        {"frame_index", observation.frame_index},
        {"sample_time_s", observation.sample_time_s},
        {"status", "ok"},
        {"diagnostics", step_result.posterior.diagnostics},
    });
  }

  const PosteriorFrame* latest = store.latest();
  if (latest == nullptr) {
    throw std::runtime_error("RuntimeEstimationService produced no posterior frames");
  }
  const nlohmann::json latest_outputs = {
      {"status", "ok"},
      {"outputs",
       {
           {"state.posterior", posteriorPortPayload(*latest)},
           {"uncertainty.posterior", uncertaintyPortPayload(*latest)},
           {"diagnostics.estimation", latest->diagnostics},
       }},
  };
  nlohmann::json outputs = nlohmann::json::object();
  outputs[stage_id] = latest_outputs;

  nlohmann::json node_snapshot = {
      {"node_id", stage_id},
      {"operator_id", jsonString(system, "estimation_system_id")},
      {"adapter_id", execution_mode == "serial_mvp" ? "runtime.estimation_service.serial" : "runtime.estimation_service.scheduled"},
      {"execution_kind", "system_service"},
      {"status", "ok"},
      {"frame_count", store.frames().size()},
      {"method", method_kind},
      {"outputs", latest_outputs.value("outputs", nlohmann::json::object())},
  };

  nlohmann::json estimation_evidence = {
      {"schema_version", "flightenv.platform.estimation_evidence.v1"},
      {"run_id", request.run_id},
      {"workflow_id", request.workflow_id},
      {"object_id", request.object_id},
      {"branch_id", request.branch_id},
      {"timeline_id", request.timeline_id},
      {"generated_at_utc", RuntimeClock::nowUtcIso()},
      {"estimation_system_id", jsonString(system, "estimation_system_id")},
      {"stage_id", stage_id},
      {"method", method_kind},
      {"execution_mode", execution_mode},
      {"model_graphs", system.value("model_graphs", nlohmann::json::object())},
      {"summary",
       {
           {"frame_count", store.frames().size()},
           {"committed_frame_count", commit_log.size()},
           {"state_dim", state_dim},
           {"failed_frame_count", 0},
           {"latest_checkpoint", latest->checkpoint_id},
           {"latest_commit_id", latest->commit_id},
           {"latest_diagnostics", latest->diagnostics},
       }},
      {"filter_commit",
       {
           {"phase_model", "predict_update_commit"},
           {"execution_mode", execution_mode},
           {"commit_barrier", "posterior_commit"},
           {"source_state_visibility", "committed_only"},
           {"committed_frame_count", commit_log.size()},
           {"events", filter_phase_events},
           {"commit_log", commit_log},
       }},
      {"sample_scheduler",
       {
           {"enabled", true},
           {"frames", sample_scheduler_frames},
       }},
      {"branches",
       {
           {"policy", branching_policy},
           {"events", branch_events},
           {"triggered_branch_count", branch_events.size()},
       }},
      {"frames", frame_evidence},
  };

  const RuntimeEvidenceWriter writer(request.run_dir);
  writer.writeJson("estimation_evidence.json", estimation_evidence);
  writer.writeJson("scheduler_timeline.json",
                   {{"schema_version", "flightenv.platform.scheduler_timeline.v1"},
                    {"run_id", request.run_id},
                    {"workflow_id", request.workflow_id},
                    {"object_id", request.object_id},
                    {"generated_at_utc", RuntimeClock::nowUtcIso()},
                    {"events", scheduler_events}});
  writer.writeJson("branch_timeline.json",
                   {{"schema_version", "flightenv.platform.branch_timeline.v1"},
                    {"run_id", request.run_id},
                    {"workflow_id", request.workflow_id},
                    {"object_id", request.object_id},
                    {"parent_branch_id", request.branch_id},
                    {"timeline_id", request.timeline_id},
                    {"generated_at_utc", RuntimeClock::nowUtcIso()},
                    {"policy", branching_policy},
                    {"events", branch_events},
                    {"summary", {{"triggered_branch_count", branch_events.size()}}}});
  writer.writeJson("runtime_node_snapshot.json",
                   {{"schema_version", "flightenv.platform.runtime_node_snapshot.v1"},
                    {"run_id", request.run_id},
                    {"workflow_id", request.workflow_id},
                    {"object_id", request.object_id},
                    {"generated_at_utc", RuntimeClock::nowUtcIso()},
                    {"nodes", nlohmann::json::array({node_snapshot})}});
  writer.writeJson("runtime_outputs.json",
                   {{"schema_version", "flightenv.platform.runtime_outputs.v1"},
                    {"run_id", request.run_id},
                    {"workflow_id", request.workflow_id},
                    {"object_id", request.object_id},
                    {"generated_at_utc", RuntimeClock::nowUtcIso()},
                    {"last_loop_iteration_index", latest->frame_index},
                    {"outputs", outputs}});
  writer.writeJson("runtime_loop_summary.json",
                   {{"schema_version", "flightenv.platform.runtime_loop_summary.v1"},
                    {"run_id", request.run_id},
                    {"workflow_id", request.workflow_id},
                    {"object_id", request.object_id},
                    {"generated_at_utc", RuntimeClock::nowUtcIso()},
                    {"iterations", loop_iterations},
                    {"summary",
                     {{"iteration_count", store.frames().size()},
                      {"committed_frame_count", commit_log.size()},
                      {"failed_nodes", 0},
                      {"estimation_method", method_kind}}}});
  writer.writeJson("state_checkpoint.json", store.toCheckpointJson(request.run_id, request.workflow_id, request.object_id));
  writer.writeJson("runtime_evidence.json",
                   {{"schema_version", "flightenv.platform.runtime_evidence.v1"},
                    {"run_id", request.run_id},
                    {"workflow_id", request.workflow_id},
                    {"object_id", request.object_id},
                    {"branch_id", request.branch_id},
                    {"timeline_id", request.timeline_id},
                    {"status", "completed"},
                    {"generated_at_utc", RuntimeClock::nowUtcIso()},
                    {"execution_backend", "native_adapter_sessions"},
                    {"runtime_core",
                     {{"estimation_service", "RuntimeEstimationService"},
                      {"estimation_execution_mode", execution_mode},
                      {"sample_scheduler", "ready_queue_sample_batch_v1"}}},
                    {"filter_commit",
                     {{"phase_model", "predict_update_commit"},
                      {"commit_barrier", "posterior_commit"},
                      {"source_state_visibility", "committed_only"},
                      {"committed_frame_count", commit_log.size()}}},
                    {"compiled_workflow_dir", request.compiled_workflow_dir.string()},
                    {"summary",
                     {{"node_count", 1},
                      {"iteration_count", store.frames().size()},
                      {"failed_nodes", 0},
                      {"estimation_system_count", 1},
                      {"estimation_method", method_kind},
                      {"sample_scheduler_frame_count", sample_scheduler_frames.size()},
                      {"committed_frame_count", commit_log.size()},
                      {"triggered_branch_count", branch_events.size()},
                      {"latest_checkpoint", latest->checkpoint_id},
                      {"latest_commit_id", latest->commit_id}}},
                    {"branches",
                     {{"policy", branching_policy},
                      {"events", branch_events},
                      {"triggered_branch_count", branch_events.size()}}},
                    {"refs",
                     {{"estimation_evidence", "estimation_evidence.json"},
                      {"branch_timeline", "branch_timeline.json"},
                      {"runtime_node_snapshot", "runtime_node_snapshot.json"},
                      {"runtime_outputs", "runtime_outputs.json"},
                      {"runtime_loop_summary", "runtime_loop_summary.json"},
                      {"state_checkpoint", "state_checkpoint.json"},
                      {"scheduler_timeline", "scheduler_timeline.json"}}}});

  RuntimeEstimationResult result;
  result.exit_code = 0;
  result.frame_count = static_cast<int>(store.frames().size());
  result.failed_frame_count = 0;
  result.summary = {
      {"status", "ok"},
      {"run_dir", request.run_dir.string()},
      {"estimation_method", method_kind},
      {"frame_count", result.frame_count},
      {"latest_checkpoint", latest->checkpoint_id},
      {"latest_commit_id", latest->commit_id},
      {"committed_frame_count", commit_log.size()},
  };
  return result;
}

}  // namespace

RuntimeEstimationResult RuntimeEstimationService::runSerial(const RuntimeEstimationRequest& request) const {
  return runEstimation(request, "serial_mvp");
}

RuntimeEstimationResult RuntimeEstimationService::runScheduled(const RuntimeEstimationRequest& request) const {
  return runEstimation(request, "scheduled_predict_update_commit_v1");
}

}  // namespace FlightEnvPlatformRuntime::estimation
