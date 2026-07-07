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
#include <cstdint>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
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
      {"estimator_state_available", frame.estimator_state.is_object() && !frame.estimator_state.empty()},
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

double jsonDouble(const nlohmann::json& value, const std::string& key, double fallback = 0.0) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
    return fallback;
  }
  const double parsed = value.at(key).get<double>();
  return std::isfinite(parsed) ? parsed : fallback;
}

bool jsonBool(const nlohmann::json& value, const std::string& key, bool fallback = false) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean()) {
    return fallback;
  }
  return value.at(key).get<bool>();
}

double finiteOrZero(double value) {
  return std::isfinite(value) ? value : 0.0;
}

double traceOf(const std::vector<double>& values) {
  double sum = 0.0;
  for (double value : values) {
    sum += finiteOrZero(value);
  }
  return sum;
}

std::string digestJson(const nlohmann::json& value) {
  const std::string text = value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << "fnv64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

int qualityRank(const std::string& status) {
  if (status == "invalid") {
    return 4;
  }
  if (status == "stale") {
    return 3;
  }
  if (status == "degraded") {
    return 2;
  }
  if (status == "warn") {
    return 1;
  }
  return 0;
}

std::string qualityStatus(int rank) {
  if (rank >= 4) {
    return "invalid";
  }
  if (rank == 3) {
    return "stale";
  }
  if (rank == 2) {
    return "degraded";
  }
  if (rank == 1) {
    return "warn";
  }
  return "ok";
}

const nlohmann::json* findJsonPath(const nlohmann::json& root, const std::string& dotted_path) {
  if (dotted_path.empty()) {
    return nullptr;
  }
  const nlohmann::json* current = &root;
  std::size_t begin = 0;
  while (begin < dotted_path.size()) {
    const std::size_t end = dotted_path.find('.', begin);
    const std::string key = dotted_path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    if (key.empty() || !current->is_object() || !current->contains(key)) {
      return nullptr;
    }
    current = &current->at(key);
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return current;
}

std::optional<double> numericJsonValue(const nlohmann::json* value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  if (value->is_number()) {
    const double parsed = value->get<double>();
    return std::isfinite(parsed) ? std::optional<double>(parsed) : std::nullopt;
  }
  if (value->is_boolean()) {
    return value->get<bool>() ? 1.0 : 0.0;
  }
  return std::nullopt;
}

std::string actionString(const nlohmann::json& actions, const std::string& key) {
  if (!actions.is_object() || !actions.contains(key) || !actions.at(key).is_string()) {
    return "";
  }
  return actions.at(key).get<std::string>();
}

void addSelectedAction(
    nlohmann::json& selected,
    std::set<std::string>& seen,
    const std::string& policy_key,
    const std::string& reason,
    const std::string& action) {
  if (action.empty()) {
    return;
  }
  const std::string dedupe_key = policy_key + "|" + reason + "|" + action;
  if (!seen.insert(dedupe_key).second) {
    return;
  }
  selected.push_back({
      {"policy_key", policy_key},
      {"reason", reason},
      {"action", action},
  });
}

bool actionBlocksDownstream(const std::string& action) {
  return action == "hold_downstream" || action == "block_decision" || action == "block_release";
}

nlohmann::json compactQualityDecision(const nlohmann::json& decision) {
  const nlohmann::json scheduler_actions = decision.value("scheduler_actions", nlohmann::json::object());
  const nlohmann::json adaptive = scheduler_actions.value("adaptive_frequency", nlohmann::json::object());
  const nlohmann::json gating = scheduler_actions.value("downstream_gating", nlohmann::json::object());
  return {
      {"enabled", decision.value("enabled", false)},
      {"status", decision.value("status", std::string("disabled"))},
      {"quality_rank", decision.value("quality_rank", 0)},
      {"decision_id", decision.value("decision_id", std::string())},
      {"selected_action_count", scheduler_actions.value("selected_action_count", 0)},
      {"adaptive_frequency",
       {
           {"enabled", adaptive.value("enabled", false)},
           {"decision", adaptive.value("decision", std::string("disabled"))},
           {"requested_period_s", adaptive.value("requested_period_s", 0.0)},
       }},
      {"downstream_gating",
       {
           {"enabled", gating.value("enabled", false)},
           {"decision", gating.value("decision", std::string("pass"))},
           {"downstream_allowed", gating.value("downstream_allowed", true)},
       }},
  };
}

nlohmann::json evaluateUncertaintyQualityPolicy(
    const nlohmann::json& policy,
    const nlohmann::json& method_config,
    const PosteriorFrame& posterior,
    const nlohmann::json& observation_timing,
    int predict_only_streak) {
  const bool enabled = policy.is_object() && policy.value("enabled", false);
  nlohmann::json decision = {
      {"schema_version", "flightenv.platform.uncertainty_quality_decision.v1"},
      {"enabled", enabled},
      {"frame_index", posterior.frame_index},
      {"sample_time_s", posterior.sample_time_s},
      {"status", enabled ? "ok" : "disabled"},
      {"quality_rank", 0},
      {"metric_snapshot", nlohmann::json::object()},
      {"metric_evaluations", nlohmann::json::array()},
      {"scheduler_actions", nlohmann::json::object()},
  };
  if (!enabled) {
    decision["decision_id"] = "uncertainty_quality.disabled.frame_" + std::to_string(posterior.frame_index);
    return decision;
  }

  nlohmann::json diagnostics =
      posterior.diagnostics.is_object() ? posterior.diagnostics : nlohmann::json::object();
  const double covariance_trace = traceOf(posterior.covariance_diag);
  diagnostics["posterior_covariance_trace"] = covariance_trace;
  if (!diagnostics.contains("posterior_cov_trace")) {
    diagnostics["posterior_cov_trace"] = covariance_trace;
  }
  diagnostics["predict_only_streak"] = predict_only_streak;
  if (!diagnostics.contains("observation_timing")) {
    diagnostics["observation_timing"] = observation_timing;
  }
  if (diagnostics.contains("ess") && diagnostics.at("ess").is_number()) {
    const double particle_count =
        diagnostics.contains("particle_count") && diagnostics.at("particle_count").is_number()
            ? diagnostics.at("particle_count").get<double>()
            : static_cast<double>(method_config.value("particle_count", 0));
    if (particle_count > 0.0) {
      diagnostics["ess_ratio"] = diagnostics.at("ess").get<double>() / particle_count;
    }
  }

  const nlohmann::json source_root = {
      {"diagnostics", {{"estimation", diagnostics}}},
      {"uncertainty",
       {{"posterior",
         {{"covariance_trace", covariance_trace},
          {"covariance_diag", vectorJson(posterior.covariance_diag)}}}}},
      {"runtime", {{"predict_only_streak", predict_only_streak}}},
  };

  int overall_rank = 0;
  bool predict_only_streak_triggered = false;
  const nlohmann::json metrics = policy.value("metrics", nlohmann::json::object());
  if (metrics.is_object()) {
    for (auto it = metrics.begin(); it != metrics.end(); ++it) {
      const std::string metric_id = it.key();
      const nlohmann::json metric_policy = it.value().is_object() ? it.value() : nlohmann::json::object();
      const std::string source = metric_policy.value("source", metric_id);
      std::optional<double> value = numericJsonValue(findJsonPath(source_root, source));
      if (!value) {
        value = numericJsonValue(findJsonPath(diagnostics, metric_id));
      }
      if (!value && metric_id == "predict_only_streak") {
        value = static_cast<double>(predict_only_streak);
      }
      if (!value && metric_id == "posterior_covariance_trace") {
        value = covariance_trace;
      }
      if (!value && metric_id == "observation_age_s") {
        value = numericJsonValue(findJsonPath(observation_timing, "observation_age_s"));
      }

      int metric_rank = 0;
      nlohmann::json triggered = nlohmann::json::array();
      if (value) {
        const double v = *value;
        auto checkAbove = [&](const std::string& key, const std::string& status) {
          if (metric_policy.contains(key) && metric_policy.at(key).is_number()) {
            const double threshold = metric_policy.at(key).get<double>();
            if (v > threshold) {
              metric_rank = std::max(metric_rank, qualityRank(status));
              triggered.push_back({{"threshold", key}, {"status", status}, {"threshold_value", threshold}});
            }
          }
        };
        auto checkBelow = [&](const std::string& key, const std::string& status) {
          if (metric_policy.contains(key) && metric_policy.at(key).is_number()) {
            const double threshold = metric_policy.at(key).get<double>();
            if (v < threshold) {
              metric_rank = std::max(metric_rank, qualityRank(status));
              triggered.push_back({{"threshold", key}, {"status", status}, {"threshold_value", threshold}});
            }
          }
        };
        checkAbove("warn_above", "warn");
        checkAbove("degrade_above", "degraded");
        checkAbove("stale_above", "stale");
        checkAbove("invalid_above", "invalid");
        checkBelow("warn_below", "warn");
        checkBelow("degrade_below", "degraded");
        checkBelow("invalid_below", "invalid");
        if (metric_policy.contains("max_consecutive") && metric_policy.at("max_consecutive").is_number_integer()) {
          const int max_consecutive = metric_policy.at("max_consecutive").get<int>();
          if (max_consecutive > 0 && static_cast<int>(v) >= max_consecutive) {
            metric_rank = std::max(metric_rank, qualityRank("degraded"));
            predict_only_streak_triggered = predict_only_streak_triggered || metric_id == "predict_only_streak";
            triggered.push_back({
                {"threshold", "max_consecutive"},
                {"status", "degraded"},
                {"threshold_value", max_consecutive},
            });
          }
        }
        decision["metric_snapshot"][metric_id] = v;
      }
      overall_rank = std::max(overall_rank, metric_rank);
      decision["metric_evaluations"].push_back({
          {"metric_id", metric_id},
          {"source", source},
          {"value_available", value.has_value()},
          {"value", value ? nlohmann::json(*value) : nlohmann::json(nullptr)},
          {"status", qualityStatus(metric_rank)},
          {"quality_rank", metric_rank},
          {"triggered_thresholds", triggered},
      });
    }
  }

  const std::string status = qualityStatus(overall_rank);
  decision["status"] = status;
  decision["quality_rank"] = overall_rank;
  decision["decision_id"] =
      "uncertainty_quality." + status + ".frame_" + std::to_string(posterior.frame_index);

  const nlohmann::json configured_actions =
      policy.value("scheduler_actions", nlohmann::json::object()).is_object()
          ? policy.value("scheduler_actions", nlohmann::json::object())
          : nlohmann::json::object();
  nlohmann::json selected_actions = nlohmann::json::array();
  std::set<std::string> seen_actions;
  if (overall_rank >= qualityRank("warn")) {
    addSelectedAction(selected_actions, seen_actions, "on_warn", status, actionString(configured_actions, "on_warn"));
  }
  if (overall_rank >= qualityRank("degraded")) {
    addSelectedAction(selected_actions, seen_actions, "on_degraded", status, actionString(configured_actions, "on_degraded"));
  }
  if (status == "stale") {
    addSelectedAction(selected_actions, seen_actions, "on_stale", status, actionString(configured_actions, "on_stale"));
  }
  if (status == "invalid") {
    addSelectedAction(selected_actions, seen_actions, "on_invalid", status, actionString(configured_actions, "on_invalid"));
  }
  if (predict_only_streak_triggered) {
    addSelectedAction(
        selected_actions,
        seen_actions,
        "on_predict_only_streak",
        "predict_only_streak",
        actionString(configured_actions, "on_predict_only_streak"));
  }

  bool downstream_allowed = true;
  for (const auto& item : selected_actions) {
    if (item.is_object() && actionBlocksDownstream(item.value("action", std::string()))) {
      downstream_allowed = false;
    }
  }

  nlohmann::json adaptive_decision = {{"enabled", false}, {"decision", "disabled"}};
  const nlohmann::json adaptive = configured_actions.value("adaptive_frequency", nlohmann::json::object());
  if (adaptive.is_object() && adaptive.value("enabled", false)) {
    const double min_period_s = std::max(0.0, jsonDouble(adaptive, "min_period_s", 0.0));
    const double max_period_s = std::max(min_period_s, jsonDouble(adaptive, "max_period_s", min_period_s));
    const double default_period_s = std::max(min_period_s, jsonDouble(adaptive, "default_period_s", max_period_s));
    double requested_period_s = default_period_s;
    std::string adaptive_status = "keep_default_period";
    if (overall_rank >= qualityRank("degraded") || predict_only_streak_triggered) {
      requested_period_s = min_period_s > 0.0 ? min_period_s : default_period_s;
      adaptive_status = "shorten_period";
    } else if (overall_rank == qualityRank("warn")) {
      requested_period_s = default_period_s > 0.0
                               ? std::max(min_period_s, default_period_s * 0.5)
                               : min_period_s;
      adaptive_status = "shorten_period";
    }
    if (max_period_s > 0.0) {
      requested_period_s = std::min(requested_period_s, max_period_s);
    }
    adaptive_decision = {
        {"enabled", true},
        {"decision", adaptive_status},
        {"min_period_s", min_period_s},
        {"max_period_s", max_period_s},
        {"default_period_s", default_period_s},
        {"requested_period_s", requested_period_s},
        {"applies_to_temporal_role", "state_filter"},
    };
  }

  nlohmann::json downstream_decision = {{"enabled", false}, {"decision", "pass"}, {"downstream_allowed", downstream_allowed}};
  const nlohmann::json gating = configured_actions.value("downstream_gating", nlohmann::json::object());
  if (gating.is_object() && !gating.empty()) {
    const std::string min_quality = gating.value("min_quality", std::string("invalid"));
    const int min_rank = qualityRank(min_quality);
    std::string gating_decision = "pass";
    if (overall_rank > min_rank) {
      downstream_allowed = false;
      gating_decision = status == "stale"
                            ? gating.value("stale_policy", std::string("hold_downstream"))
                            : std::string("block_decision");
    }
    downstream_decision = {
        {"enabled", true},
        {"decision", gating_decision},
        {"downstream_allowed", downstream_allowed},
        {"min_quality", min_quality},
        {"affected_temporal_roles", gating.value("affected_temporal_roles", nlohmann::json::array())},
        {"stale_policy", gating.value("stale_policy", std::string("hold_downstream"))},
    };
  }

  decision["scheduler_actions"] = {
      {"selected_actions", selected_actions},
      {"selected_action_count", selected_actions.size()},
      {"adaptive_frequency", adaptive_decision},
      {"downstream_gating", downstream_decision},
  };
  if (policy.value("evidence", nlohmann::json::object()).value("digest_inputs", true)) {
    decision["decision_digest_algorithm"] = "fnv64";
    decision["decision_digest"] = digestJson({
        {"metric_snapshot", decision["metric_snapshot"]},
        {"status", decision["status"]},
        {"scheduler_actions", decision["scheduler_actions"]},
    });
  }
  return decision;
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
      {"forecast_state_access", "committed_checkpoint_read_only"},
      {"forecast_mutates_online_posterior", false},
  };
}

std::vector<double> doubleVectorFromJson(const nlohmann::json& values) {
  std::vector<double> out;
  if (!values.is_array()) {
    return out;
  }
  out.reserve(values.size());
  for (const auto& item : values) {
    if (!item.is_number()) {
      out.push_back(0.0);
      continue;
    }
    const double value = item.get<double>();
    out.push_back(std::isfinite(value) ? value : 0.0);
  }
  return out;
}

std::vector<std::string> stringVectorFromJson(const nlohmann::json& values) {
  std::vector<std::string> out;
  if (!values.is_array()) {
    return out;
  }
  out.reserve(values.size());
  for (const auto& item : values) {
    out.push_back(item.is_string() ? item.get<std::string>() : std::string());
  }
  return out;
}

std::vector<std::string> defaultLabels(std::size_t state_dim) {
  std::vector<std::string> labels;
  labels.reserve(state_dim);
  for (std::size_t i = 0; i < state_dim; ++i) {
    labels.push_back("state_" + std::to_string(i));
  }
  return labels;
}

std::size_t resolveStateDim(const RuntimeObservationInbox& inbox) {
  for (const RuntimeObservationFrame& frame : inbox.frames()) {
    if (!frame.values.empty()) {
      return frame.values.size();
    }
  }
  return 1;
}

double resolveLateObservationTolerance(const nlohmann::json& system) {
  const nlohmann::json diagnostics = system.value("diagnostics_policy", nlohmann::json::object());
  if (diagnostics.is_object() && diagnostics.contains("late_observation_tolerance_s") &&
      diagnostics.at("late_observation_tolerance_s").is_number()) {
    return std::max(0.0, diagnostics.at("late_observation_tolerance_s").get<double>());
  }
  return 0.0;
}

nlohmann::json observationTimingEvidence(
    const RuntimeObservationFrame& observation,
    double late_tolerance_s,
    double previous_sample_time_s,
    bool has_previous_sample_time) {
  const bool late_by_arrival =
      observation.has_arrival_time &&
      observation.arrival_time_s > observation.sample_time_s + late_tolerance_s;
  const bool late = observation.explicit_late_observation || late_by_arrival;
  const bool missing = observation.missing_observation;
  const double sample_delta_s = has_previous_sample_time
                                    ? observation.sample_time_s - previous_sample_time_s
                                    : 0.0;
  return {
      {"event", "observation_timing"},
      {"event_kind", "observation_timing"},
      {"runtime_event_kind", "observation_timing"},
      {"frame_index", observation.frame_index},
      {"sample_time_s", observation.sample_time_s},
      {"arrival_time_s", observation.arrival_time_s},
      {"has_arrival_time", observation.has_arrival_time},
      {"late_observation", late},
      {"late_by_arrival_time", late_by_arrival},
      {"late_observation_declared", observation.explicit_late_observation},
      {"missing_observation", missing},
      {"observation_status", missing ? "missing" : (late ? "late" : "available")},
      {"late_tolerance_s", late_tolerance_s},
      {"observation_age_s", observation.has_arrival_time ? observation.arrival_time_s - observation.sample_time_s : 0.0},
      {"sample_delta_s", sample_delta_s},
      {"has_previous_sample_time", has_previous_sample_time},
      {"value_count", observation.values.size()},
      {"source_summary", observation.source_summary},
  };
}

void attachObservationTiming(nlohmann::json& event, const nlohmann::json& timing) {
  event["observation_status"] = timing.value("observation_status", std::string("available"));
  event["missing_observation"] = timing.value("missing_observation", false);
  event["late_observation"] = timing.value("late_observation", false);
  event["arrival_time_s"] = timing.value("arrival_time_s", 0.0);
  event["observation_age_s"] = timing.value("observation_age_s", 0.0);
  event["sample_delta_s"] = timing.value("sample_delta_s", 0.0);
}

std::optional<PosteriorFrame> restorePosteriorFrame(
    const RuntimeEstimationRequest& request,
    IEstimatorMethod& method) {
  const nlohmann::json checkpoint_doc = request.resume_state_checkpoint;
  if (!checkpoint_doc.is_object()) {
    return std::nullopt;
  }
  nlohmann::json selected = nlohmann::json::object();
  const nlohmann::json checkpoints = checkpoint_doc.value("checkpoints", nlohmann::json::array());
  if (checkpoints.is_array()) {
    for (const auto& item : checkpoints) {
      if (!item.is_object() || !item.value("committed", false)) {
        continue;
      }
      if (!request.resume_checkpoint_id.empty() &&
          item.value("checkpoint_id", std::string()) != request.resume_checkpoint_id) {
        continue;
      }
      selected = item;
    }
  } else if (checkpoint_doc.contains("checkpoint_id")) {
    selected = checkpoint_doc;
  }
  if (!selected.is_object() || selected.empty()) {
    return std::nullopt;
  }
  PosteriorFrame frame;
  frame.frame_index = jsonInt(selected, "frame_index", 0);
  frame.sample_time_s = jsonDouble(selected, "sample_time_s", 0.0);
  frame.checkpoint_id = jsonString(selected, "checkpoint_id");
  frame.commit_id = jsonString(selected, "commit_id");
  frame.committed = selected.value("committed", true);
  frame.value_labels = stringVectorFromJson(selected.value("labels", nlohmann::json::array()));
  frame.state_mean = doubleVectorFromJson(selected.value("state_mean", nlohmann::json::array()));
  frame.covariance_diag = doubleVectorFromJson(selected.value("covariance_diag", nlohmann::json::array()));
  frame.estimator_state = selected.value("estimator_state", nlohmann::json::object());
  frame.diagnostics = selected.value("diagnostics", nlohmann::json::object());
  if (!frame.estimator_state.empty()) {
    method.restoreState(frame.estimator_state);
  }
  return frame;
}

PosteriorFrame predictOnlyPosterior(
    const RuntimeObservationFrame& observation,
    const PosteriorFrame* previous,
    std::size_t state_dim,
    const std::string& method_kind,
    const nlohmann::json& estimator_state) {
  PosteriorFrame posterior;
  posterior.frame_index = observation.frame_index;
  posterior.sample_time_s = observation.sample_time_s;
  posterior.checkpoint_id = "posterior." + method_kind + ".frame_" +
                            std::to_string(observation.frame_index) + ".predict_only";
  posterior.value_labels = previous && !previous->value_labels.empty()
                               ? previous->value_labels
                               : defaultLabels(state_dim);
  posterior.state_mean = previous && !previous->state_mean.empty()
                             ? previous->state_mean
                             : std::vector<double>(state_dim, 0.0);
  posterior.covariance_diag = previous && !previous->covariance_diag.empty()
                                  ? previous->covariance_diag
                                  : std::vector<double>(state_dim, 1.0);
  posterior.state_mean.resize(state_dim, 0.0);
  posterior.covariance_diag.resize(state_dim, 1.0);
  for (double& value : posterior.covariance_diag) {
    value = std::max(1.0e-9, value + 1.0e-6);
  }
  posterior.estimator_state = estimator_state;
  posterior.diagnostics = {
      {"predict_only", true},
      {"missing_observation", true},
      {"posterior_checkpoint", posterior.checkpoint_id},
      {"previous_checkpoint", previous ? previous->checkpoint_id : std::string()},
      {"state_dim", state_dim},
  };
  return posterior;
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
  const std::size_t state_dim = std::max<std::size_t>(1, resolveStateDim(inbox));
  std::unique_ptr<IEstimatorMethod> method = EstimatorMethodRegistry::create(method_kind);
  method->configure(method_config, state_dim);
  const std::optional<PosteriorFrame> resumed_frame = restorePosteriorFrame(request, *method);
  const bool resumed_from_checkpoint = resumed_frame.has_value();

  SampleSetStore store;
  SampleScheduler sample_scheduler;
  nlohmann::json frame_evidence = nlohmann::json::array();
  nlohmann::json sample_scheduler_frames = nlohmann::json::array();
  nlohmann::json scheduler_events = nlohmann::json::array();
  nlohmann::json loop_iterations = nlohmann::json::array();
  nlohmann::json branch_events = nlohmann::json::array();
  nlohmann::json filter_phase_events = nlohmann::json::array();
  nlohmann::json commit_log = nlohmann::json::array();
  nlohmann::json observation_timing_events = nlohmann::json::array();
  const nlohmann::json workflow = workflowFromSnapshot(request.workflow_snapshot);
  const nlohmann::json branching_policy = workflow.value("branching_policy", nlohmann::json::object());
  const std::string stage_id = jsonString(system, "compiled_stage_id", "online_current.online_estimation");
  const nlohmann::json profile_schedule_overrides =
      system.value("profile_schedule_overrides", nlohmann::json::array());
  const nlohmann::json uncertainty_quality_policy =
      system.value("uncertainty_quality_policy", nlohmann::json::object());
  const bool uncertainty_quality_policy_enabled =
      uncertainty_quality_policy.is_object() && uncertainty_quality_policy.value("enabled", false);
  const double late_observation_tolerance_s = resolveLateObservationTolerance(system);
  int missing_observation_count = 0;
  int late_observation_count = 0;
  int predict_only_frame_count = 0;
  int update_frame_count = 0;
  int variable_sample_delta_count = 0;
  int predict_only_streak = 0;
  int uncertainty_quality_decision_count = 0;
  int adaptive_frequency_decision_count = 0;
  int downstream_gate_hold_count = 0;
  int downstream_gate_block_count = 0;
  int branch_hold_count = 0;
  std::map<std::string, int> uncertainty_quality_status_counts;
  nlohmann::json uncertainty_quality_events = nlohmann::json::array();
  nlohmann::json latest_uncertainty_quality_decision = nlohmann::json::object();
  bool has_previous_sample_time = resumed_from_checkpoint;
  double previous_sample_time_s = resumed_from_checkpoint ? resumed_frame->sample_time_s : 0.0;
  scheduler_events.push_back({
      {"timestamp_utc", RuntimeClock::nowUtcIso()},
      {"event", "estimation_session_begin"},
      {"stage_id", stage_id},
      {"estimation_system_id", jsonString(system, "estimation_system_id")},
      {"method", method_kind},
      {"frame_count", inbox.size()},
      {"execution_mode", execution_mode},
      {"resumed_from_checkpoint", resumed_from_checkpoint},
  });
  if (resumed_from_checkpoint) {
    scheduler_events.push_back({
        {"timestamp_utc", RuntimeClock::nowUtcIso()},
        {"event", "filter_resume_checkpoint"},
        {"event_kind", "filter_resume_checkpoint"},
        {"runtime_event_kind", "filter_resume_checkpoint"},
        {"stage_id", stage_id},
        {"status", "restored"},
        {"checkpoint_id", resumed_frame->checkpoint_id},
        {"posterior_commit_id", resumed_frame->commit_id},
        {"frame_index", resumed_frame->frame_index},
        {"sample_time_s", resumed_frame->sample_time_s},
        {"committed", resumed_frame->committed},
        {"estimator_state_restored", resumed_frame->estimator_state.is_object() && !resumed_frame->estimator_state.empty()},
        {"source_state_visibility", "committed_only"},
    });
  }

  for (const RuntimeObservationFrame& observation : inbox.frames()) {
    nlohmann::json frame_phase_events = nlohmann::json::array();
    nlohmann::json observation_timing = observationTimingEvidence(
        observation,
        late_observation_tolerance_s,
        previous_sample_time_s,
        has_previous_sample_time);
    observation_timing["timestamp_utc"] = RuntimeClock::nowUtcIso();
    observation_timing["stage_id"] = stage_id;
    observation_timing_events.push_back(observation_timing);
    scheduler_events.push_back(observation_timing);
    const bool missing_observation = observation_timing.value("missing_observation", false);
    const bool late_observation = observation_timing.value("late_observation", false);
    if (missing_observation) {
      ++missing_observation_count;
    }
    if (late_observation) {
      ++late_observation_count;
    }
    const double current_sample_delta_s = observation_timing.value("sample_delta_s", 0.0);
    if (has_previous_sample_time && std::abs(current_sample_delta_s) > 1.0e-12) {
      if (observation_timing_events.size() > 2) {
        const auto& previous_timing = observation_timing_events.at(observation_timing_events.size() - 2);
        const double previous_delta = previous_timing.value("sample_delta_s", current_sample_delta_s);
        if (std::abs(previous_delta) > 1.0e-12 &&
            std::abs(previous_delta - current_sample_delta_s) > 1.0e-9) {
          ++variable_sample_delta_count;
        }
      }
    }
    scheduler_events.push_back({
        {"timestamp_utc", RuntimeClock::nowUtcIso()},
        {"event", "estimation_frame_begin"},
        {"stage_id", stage_id},
        {"frame_index", observation.frame_index},
        {"sample_time_s", observation.sample_time_s},
        {"observation_timing", observation_timing},
    });
    const PosteriorFrame* previous_frame = store.latest();
    if (previous_frame == nullptr && resumed_from_checkpoint) {
      previous_frame = &(*resumed_frame);
    }
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
    attachObservationTiming(predict_event, observation_timing);
    predict_event["sample_scheduler"] = compactSampleScheduleDiagnostics(sample_schedule);
    scheduler_events.push_back(predict_event);
    filter_phase_events.push_back(predict_event);
    frame_phase_events.push_back(predict_event);
    EstimatorStepResult step_result;
    if (missing_observation) {
      ++predict_only_frame_count;
      ++predict_only_streak;
      EstimatorStepRequest step_request;
      step_request.observation = observation;
      step_request.previous = previous_frame;
      step_result = method->predictOnly(step_request);
      step_result.posterior.estimator_state = method->snapshotState();
    } else {
      predict_only_streak = 0;
      ++update_frame_count;
      EstimatorStepRequest step_request;
      step_request.observation = observation;
      step_request.previous = previous_frame;
      step_result = method->step(step_request);
      step_result.posterior.estimator_state = method->snapshotState();
    }
    step_result.posterior.commit_id = posteriorCommitId(request, observation.frame_index);
    step_result.posterior.committed = false;
    step_result.posterior.diagnostics["sample_scheduler"] =
        compactSampleScheduleDiagnostics(sample_schedule);
    step_result.posterior.diagnostics["posterior_commit_id"] = step_result.posterior.commit_id;
    step_result.posterior.diagnostics["posterior_committed"] = true;
    step_result.posterior.diagnostics["filter_execution_mode"] = execution_mode;
    step_result.posterior.diagnostics["missing_observation"] = missing_observation;
    step_result.posterior.diagnostics["late_observation"] = late_observation;
    step_result.posterior.diagnostics["observation_status"] =
        observation_timing.value("observation_status", std::string("available"));
    step_result.posterior.diagnostics["predict_only"] = missing_observation;
    step_result.posterior.diagnostics["predict_only_streak"] = predict_only_streak;
    step_result.posterior.diagnostics["observation_timing"] = observation_timing;
    step_result.posterior.diagnostics["posterior_covariance_trace"] =
        traceOf(step_result.posterior.covariance_diag);
    if (!step_result.posterior.diagnostics.contains("posterior_cov_trace")) {
      step_result.posterior.diagnostics["posterior_cov_trace"] =
          step_result.posterior.diagnostics["posterior_covariance_trace"];
    }
    if (step_result.posterior.diagnostics.contains("ess") &&
        step_result.posterior.diagnostics.at("ess").is_number()) {
      const double particle_count =
          step_result.posterior.diagnostics.contains("particle_count") &&
                  step_result.posterior.diagnostics.at("particle_count").is_number()
              ? step_result.posterior.diagnostics.at("particle_count").get<double>()
              : static_cast<double>(method_config.value("particle_count", 0));
      if (particle_count > 0.0) {
        step_result.posterior.diagnostics["ess_ratio"] =
            step_result.posterior.diagnostics.at("ess").get<double>() / particle_count;
      }
    }
    nlohmann::json uncertainty_quality_decision = evaluateUncertaintyQualityPolicy(
        uncertainty_quality_policy,
        method_config,
        step_result.posterior,
        observation_timing,
        predict_only_streak);
    uncertainty_quality_decision["timestamp_utc"] = RuntimeClock::nowUtcIso();
    uncertainty_quality_decision["event"] = "uncertainty_quality_decision";
    uncertainty_quality_decision["event_kind"] = "uncertainty_quality_decision";
    uncertainty_quality_decision["runtime_event_kind"] = "uncertainty_quality_decision";
    uncertainty_quality_decision["stage_id"] = stage_id;
    latest_uncertainty_quality_decision = uncertainty_quality_decision;
    if (uncertainty_quality_policy_enabled) {
      ++uncertainty_quality_decision_count;
      const std::string quality_status =
          uncertainty_quality_decision.value("status", std::string("ok"));
      ++uncertainty_quality_status_counts[quality_status];
      const nlohmann::json quality_actions =
          uncertainty_quality_decision.value("scheduler_actions", nlohmann::json::object());
      const nlohmann::json adaptive =
          quality_actions.value("adaptive_frequency", nlohmann::json::object());
      if (adaptive.value("enabled", false)) {
        ++adaptive_frequency_decision_count;
      }
      const nlohmann::json gating =
          quality_actions.value("downstream_gating", nlohmann::json::object());
      if (gating.value("enabled", false) && !gating.value("downstream_allowed", true)) {
        const std::string gating_decision = gating.value("decision", std::string());
        if (gating_decision == "hold_downstream") {
          ++downstream_gate_hold_count;
        } else {
          ++downstream_gate_block_count;
        }
      }
    }
    uncertainty_quality_events.push_back(uncertainty_quality_decision);
    scheduler_events.push_back(uncertainty_quality_decision);
    step_result.posterior.diagnostics["uncertainty_quality"] =
        compactQualityDecision(uncertainty_quality_decision);
    nlohmann::json update_event = filterPhaseEvent(
        "filter_update",
        missing_observation ? "skipped" : "completed",
        stage_id,
        observation,
        request.branch_id,
        request.timeline_id,
        previous_checkpoint,
        step_result.posterior.checkpoint_id,
        "");
    attachObservationTiming(update_event, observation_timing);
    update_event["diagnostics"] = step_result.posterior.diagnostics;
    update_event["uncertainty_quality"] = compactQualityDecision(uncertainty_quality_decision);
    if (missing_observation) {
      update_event["reason"] = "missing_observation";
      update_event["predict_only"] = true;
    }
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
    attachObservationTiming(commit_event, observation_timing);
    commit_event["checkpoint_id"] = step_result.posterior.checkpoint_id;
    commit_event["committed"] = true;
    commit_event["predict_only"] = missing_observation;
    commit_event["estimator_state_checkpointed"] =
        step_result.posterior.estimator_state.is_object() && !step_result.posterior.estimator_state.empty();
    commit_event["uncertainty_quality"] = compactQualityDecision(uncertainty_quality_decision);
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
        {"predict_only", missing_observation},
        {"missing_observation", missing_observation},
        {"late_observation", late_observation},
        {"observation_status", observation_timing.value("observation_status", std::string("available"))},
    });
    frame_evidence.push_back(frameEvidence(step_result.posterior));
    const int processed_frame_count = static_cast<int>(store.frames().size());
    if (shouldTriggerBranch(branching_policy, processed_frame_count, static_cast<int>(branch_events.size()))) {
      const nlohmann::json gating =
          uncertainty_quality_decision.value("scheduler_actions", nlohmann::json::object())
              .value("downstream_gating", nlohmann::json::object());
      const bool downstream_allowed =
          !uncertainty_quality_policy_enabled || gating.value("downstream_allowed", true);
      if (downstream_allowed) {
        const nlohmann::json branch_event =
            branchTriggerEvent(request, branching_policy, step_result.posterior, static_cast<int>(branch_events.size()));
        branch_events.push_back(branch_event);
        scheduler_events.push_back(branch_event);
      } else {
        ++branch_hold_count;
        scheduler_events.push_back({
            {"timestamp_utc", RuntimeClock::nowUtcIso()},
            {"event", "branch_held_by_uncertainty_quality"},
            {"event_kind", "branch_held_by_uncertainty_quality"},
            {"runtime_event_kind", "branch_held_by_uncertainty_quality"},
            {"stage_id", stage_id},
            {"frame_index", observation.frame_index},
            {"sample_time_s", observation.sample_time_s},
            {"checkpoint_id", step_result.posterior.checkpoint_id},
            {"posterior_commit_id", step_result.posterior.commit_id},
            {"uncertainty_quality", compactQualityDecision(uncertainty_quality_decision)},
        });
      }
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
        {"predict_only", missing_observation},
        {"missing_observation", missing_observation},
        {"late_observation", late_observation},
        {"uncertainty_quality", compactQualityDecision(uncertainty_quality_decision)},
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
        {"observation_timing", observation_timing},
    });
    previous_sample_time_s = observation.sample_time_s;
    has_previous_sample_time = true;
  }

  const PosteriorFrame* latest = store.latest();
  if (latest == nullptr) {
    throw std::runtime_error("RuntimeEstimationService produced no posterior frames");
  }
  nlohmann::json uncertainty_quality_status_counts_json = nlohmann::json::object();
  for (const auto& item : uncertainty_quality_status_counts) {
    uncertainty_quality_status_counts_json[item.first] = item.second;
  }
  const nlohmann::json uncertainty_quality_summary = {
      {"enabled", uncertainty_quality_policy_enabled},
      {"decision_count", uncertainty_quality_decision_count},
      {"status_counts", uncertainty_quality_status_counts_json},
      {"latest_status",
       latest_uncertainty_quality_decision.value(
           "status",
           std::string(uncertainty_quality_policy_enabled ? "ok" : "disabled"))},
      {"latest_quality_rank", latest_uncertainty_quality_decision.value("quality_rank", 0)},
      {"adaptive_frequency_decision_count", adaptive_frequency_decision_count},
      {"downstream_gate_hold_count", downstream_gate_hold_count},
      {"downstream_gate_block_count", downstream_gate_block_count},
      {"branch_hold_count", branch_hold_count},
      {"policy_digest",
       uncertainty_quality_policy_enabled ? digestJson(uncertainty_quality_policy) : std::string()},
  };
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
      {"profile_schedule_overrides", profile_schedule_overrides},
      {"summary",
       {
           {"frame_count", store.frames().size()},
           {"committed_frame_count", commit_log.size()},
           {"profile_schedule_override_count",
            profile_schedule_overrides.is_array() ? profile_schedule_overrides.size() : 0},
           {"state_dim", state_dim},
           {"failed_frame_count", 0},
           {"missing_observation_count", missing_observation_count},
           {"late_observation_count", late_observation_count},
           {"predict_only_frame_count", predict_only_frame_count},
           {"update_frame_count", update_frame_count},
           {"variable_sample_delta_count", variable_sample_delta_count},
           {"uncertainty_quality", uncertainty_quality_summary},
           {"resumed_from_checkpoint", resumed_from_checkpoint},
           {"resume_checkpoint_id", resumed_from_checkpoint ? resumed_frame->checkpoint_id : std::string()},
           {"latest_checkpoint", latest->checkpoint_id},
           {"latest_commit_id", latest->commit_id},
           {"latest_diagnostics", latest->diagnostics},
       }},
      {"observation_timing",
       {
           {"late_observation_tolerance_s", late_observation_tolerance_s},
           {"missing_observation_count", missing_observation_count},
           {"late_observation_count", late_observation_count},
           {"variable_sample_delta_count", variable_sample_delta_count},
           {"events", observation_timing_events},
       }},
      {"filter_commit",
       {
           {"phase_model", "predict_update_commit"},
           {"execution_mode", execution_mode},
           {"commit_barrier", "posterior_commit"},
           {"source_state_visibility", "committed_only"},
           {"committed_frame_count", commit_log.size()},
           {"predict_only_frame_count", predict_only_frame_count},
           {"update_frame_count", update_frame_count},
           {"events", filter_phase_events},
           {"commit_log", commit_log},
       }},
      {"checkpoint_resume",
       {
           {"resumed_from_checkpoint", resumed_from_checkpoint},
           {"checkpoint_id", resumed_from_checkpoint ? resumed_frame->checkpoint_id : std::string()},
           {"posterior_commit_id", resumed_from_checkpoint ? resumed_frame->commit_id : std::string()},
           {"estimator_state_restored",
            resumed_from_checkpoint && resumed_frame->estimator_state.is_object() && !resumed_frame->estimator_state.empty()},
           {"source_state_visibility", "committed_only"},
       }},
      {"uncertainty_quality",
       {
           {"policy", uncertainty_quality_policy},
           {"summary", uncertainty_quality_summary},
           {"latest_decision", latest_uncertainty_quality_decision},
           {"events", uncertainty_quality_events},
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
  writer.writeJson("uncertainty_quality_evidence.json",
                   {{"schema_version", "flightenv.platform.uncertainty_quality_evidence.v1"},
                    {"run_id", request.run_id},
                    {"workflow_id", request.workflow_id},
                    {"object_id", request.object_id},
                    {"branch_id", request.branch_id},
                    {"timeline_id", request.timeline_id},
                    {"generated_at_utc", RuntimeClock::nowUtcIso()},
                    {"estimation_system_id", jsonString(system, "estimation_system_id")},
                    {"stage_id", stage_id},
                    {"policy", uncertainty_quality_policy},
                    {"summary", uncertainty_quality_summary},
                    {"latest_decision", latest_uncertainty_quality_decision},
                    {"events", uncertainty_quality_events}});
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
                      {"estimation_method", method_kind},
                      {"uncertainty_quality", uncertainty_quality_summary}}}});
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
                      {"committed_frame_count", commit_log.size()},
                      {"predict_only_frame_count", predict_only_frame_count},
                      {"update_frame_count", update_frame_count}}},
                    {"uncertainty_quality", uncertainty_quality_summary},
                    {"checkpoint_resume",
                     {{"resumed_from_checkpoint", resumed_from_checkpoint},
                      {"checkpoint_id", resumed_from_checkpoint ? resumed_frame->checkpoint_id : std::string()},
                      {"posterior_commit_id", resumed_from_checkpoint ? resumed_frame->commit_id : std::string()},
                      {"estimator_state_restored",
                       resumed_from_checkpoint && resumed_frame->estimator_state.is_object() && !resumed_frame->estimator_state.empty()}}},
                    {"compiled_workflow_dir", request.compiled_workflow_dir.string()},
                    {"summary",
                     {{"node_count", 1},
                      {"iteration_count", store.frames().size()},
                      {"failed_nodes", 0},
                      {"estimation_system_count", 1},
                      {"estimation_method", method_kind},
                      {"sample_scheduler_frame_count", sample_scheduler_frames.size()},
                      {"committed_frame_count", commit_log.size()},
                      {"missing_observation_count", missing_observation_count},
                      {"late_observation_count", late_observation_count},
                      {"predict_only_frame_count", predict_only_frame_count},
                      {"update_frame_count", update_frame_count},
                      {"variable_sample_delta_count", variable_sample_delta_count},
                      {"uncertainty_quality_policy_enabled", uncertainty_quality_policy_enabled},
                      {"uncertainty_quality_decision_count", uncertainty_quality_decision_count},
                      {"uncertainty_quality_latest_status",
                       uncertainty_quality_summary.value("latest_status", std::string())},
                      {"uncertainty_quality_adaptive_frequency_decision_count",
                       adaptive_frequency_decision_count},
                      {"uncertainty_quality_downstream_gate_hold_count",
                       downstream_gate_hold_count},
                      {"uncertainty_quality_downstream_gate_block_count",
                       downstream_gate_block_count},
                      {"uncertainty_quality_branch_hold_count", branch_hold_count},
                      {"resumed_from_checkpoint", resumed_from_checkpoint},
                      {"estimation_profile_schedule_override_count",
                       profile_schedule_overrides.is_array() ? profile_schedule_overrides.size() : 0},
                      {"triggered_branch_count", branch_events.size()},
                      {"latest_checkpoint", latest->checkpoint_id},
                      {"latest_commit_id", latest->commit_id}}},
                    {"branches",
                     {{"policy", branching_policy},
                      {"events", branch_events},
                      {"triggered_branch_count", branch_events.size()}}},
                    {"refs",
                     {{"estimation_evidence", "estimation_evidence.json"},
                      {"uncertainty_quality_evidence", "uncertainty_quality_evidence.json"},
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
      {"missing_observation_count", missing_observation_count},
      {"late_observation_count", late_observation_count},
      {"predict_only_frame_count", predict_only_frame_count},
      {"uncertainty_quality", uncertainty_quality_summary},
      {"resumed_from_checkpoint", resumed_from_checkpoint},
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
