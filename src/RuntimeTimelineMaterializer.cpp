#include "FlightEnvPlatformRuntime/RuntimeTimelineMaterializer.hpp"

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

bool startsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

nlohmann::json RuntimeTimelineMaterializer::makeBranchStep(
    const nlohmann::json& iteration,
    const std::string& branch_id,
    int fallback_step_index) {
  nlohmann::json step = iteration.is_object() ? iteration : nlohmann::json::object();
  const int step_index =
      jsonInt(step, "iteration_index", jsonInt(step, "loop_iteration_index", fallback_step_index));
  const double run_time_s = jsonDouble(step, "run_time_s", static_cast<double>(step_index));
  const double public_time_s = jsonDouble(step, "public_output_time_s", run_time_s);
  step["branch_id"] = branch_id;
  step["step_index"] = step_index;
  step["public_time_s"] = public_time_s;
  if (!step.contains("time_point") || !step["time_point"].is_object()) {
    step["time_point"] = {
        {"run_time_s", public_time_s},
        {"tick_index", step_index + 1},
        {"source_time_s", public_time_s},
    };
  }
  if (!step.contains("time_summary") || !step["time_summary"].is_object()) {
    step["time_summary"] = nlohmann::json::object();
  }
  step["time_summary"]["base_dt_s"] = jsonDouble(step, "base_dt_s", 0.0);
  step["time_summary"]["output_period_s"] = jsonDouble(step, "output_period_s", 0.0);
  step["time_summary"]["effective_delta_t_s_by_node"] =
      step.value("effective_delta_t_s_by_node", nlohmann::json::object());
  step["time_summary"]["held_output_count"] = jsonInt(step, "held_output_count", 0);
  step["time_summary"]["held_outputs"] = step.value("held_outputs", nlohmann::json::array());
  return step;
}

nlohmann::json RuntimeTimelineMaterializer::makeArtifactRef(
    const nlohmann::json& data_plane_entry,
    const std::string& branch_id) {
  nlohmann::json entry = data_plane_entry.is_object() ? data_plane_entry : nlohmann::json::object();
  const int step_index = jsonInt(entry, "loop_iteration_index", -1);
  entry["branch_id"] = branch_id;
  if (step_index >= 0) {
    entry["step_index"] = step_index;
  }
  const nlohmann::json time_point = entry.value("time_point", nlohmann::json::object());
  entry["public_time_s"] = jsonDouble(entry,
                                      "public_time_s",
                                      jsonDouble(time_point,
                                                 "run_time_s",
                                                 step_index >= 0 ? static_cast<double>(step_index) : 0.0));
  return entry;
}

bool RuntimeTimelineMaterializer::isQoiRef(const nlohmann::json& timeline_ref) {
  const std::string port_id = jsonString(timeline_ref, "port_id");
  const std::string value_kind = jsonString(timeline_ref, "value_kind");
  const std::string role = jsonString(timeline_ref, "role", jsonString(timeline_ref, "display_role"));
  return startsWith(port_id, "qoi.") || value_kind == "decision" || value_kind == "qoi" ||
         startsWith(role, "qoi.");
}

}  // namespace FlightEnvPlatformRuntime
