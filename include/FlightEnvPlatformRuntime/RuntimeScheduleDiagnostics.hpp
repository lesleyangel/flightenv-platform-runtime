#pragma once

/**
 * @file RuntimeScheduleDiagnostics.hpp
 * @brief Builds platform-level schedule diagnostics from scheduler artifacts.
 *
 * The diagnostics are intentionally derived from scheduler_table.json and the
 * deterministic due-time trace. They describe rate topology, deterministic
 * multitasking batches, and timing budget checks without encoding object
 * semantics or changing operator execution behavior.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace FlightEnvPlatformRuntime {

class RuntimeScheduleDiagnostics {
 public:
  RuntimeScheduleDiagnostics(
      nlohmann::json scheduler_table,
      nlohmann::json due_time_trace,
      int worker_count)
      : scheduler_table_(std::move(scheduler_table)),
        due_time_trace_(std::move(due_time_trace)),
        worker_count_(std::max(1, worker_count)) {}

  nlohmann::json build() const {
    const std::vector<Row> rows = parseRows();
    const std::map<std::string, Row> row_by_id = rowsById(rows);
    nlohmann::json rate_graph = buildRateGraph(rows, row_by_id);
    nlohmann::json multitasking = buildMultitasking(row_by_id);
    nlohmann::json timing = buildTiming(row_by_id);
    nlohmann::json stable_summary = {
        {"node_count", rate_graph.at("summary").value("node_count", 0)},
        {"rate_graph_edge_count", rate_graph.at("summary").value("edge_count", 0)},
        {"transition_edge_count", rate_graph.at("summary").value("transition_edge_count", 0)},
        {"cross_rate_edge_count", rate_graph.at("summary").value("cross_rate_edge_count", 0)},
        {"distinct_period_count", rate_graph.at("summary").value("distinct_period_count", 0)},
        {"multitasking_batch_count", multitasking.at("summary").value("batch_count", 0)},
        {"parallelizable_batch_count", multitasking.at("summary").value("parallelizable_batch_count", 0)},
        {"deterministic_order", multitasking.at("summary").value("deterministic_order", false)},
        {"multitasking_dependency_violation_count",
         multitasking.at("summary").value("dependency_violation_count", 0)},
        {"deadline_check_event_count", timing.at("summary").value("deadline_check_event_count", 0)},
        {"deadline_miss_count", timing.at("summary").value("deadline_miss_count", 0)},
        {"overrun_count", timing.at("summary").value("overrun_count", 0)},
        {"jitter_violation_count", timing.at("summary").value("jitter_violation_count", 0)},
        {"max_abs_jitter_s", timing.at("summary").value("max_abs_jitter_s", 0.0)},
        {"worker_count", worker_count_},
    };
    stable_summary["diagnostics_digest_algorithm"] = "fnv64";
    stable_summary["diagnostics_digest"] = digestJson({
        {"summary", stable_summary},
        {"rate_graph_summary", rate_graph.at("summary")},
        {"multitasking_summary", multitasking.at("summary")},
        {"timing_summary", timing.at("summary")},
    });

    return {
        {"schema_version", "flightenv.platform.scheduler_diagnostics.v1"},
        {"source_scheduler_table_schema", scheduler_table_.value("schema_version", std::string())},
        {"source_due_time_trace_schema", due_time_trace_.value("schema_version", std::string())},
        {"source_table_id", scheduler_table_.value("table_id", std::string())},
        {"workflow_id", scheduler_table_.value("workflow_id", std::string())},
        {"object_id", scheduler_table_.value("object_id", std::string())},
        {"phase", scheduler_table_.value("phase", std::string())},
        {"summary", stable_summary},
        {"rate_graph", rate_graph},
        {"deterministic_multitasking", multitasking},
        {"deadline_overrun_jitter", timing},
    };
  }

 private:
  struct Row {
    std::string entry_id;
    std::string node_id;
    std::string operator_id;
    std::string trigger;
    std::string capacity_group;
    std::string resource_lock_mode;
    bool can_run_parallel = false;
    double period_s = 0.0;
    double offset_s = 0.0;
    double due_time_s = 0.0;
    double deadline_s = 0.0;
    double timeout_s = 0.0;
    int scheduling_level = 0;
    int priority_rank = 0;
    int dispatch_order = 0;
    int source_order = 0;
    std::vector<std::string> depends_on;
    nlohmann::json depends_on_json = nlohmann::json::array();
    nlohmann::json input_alignment = nlohmann::json::array();
  };

  static constexpr double kEpsilon = 1.0e-9;

  static std::string jsonString(
      const nlohmann::json& value,
      const std::string& key,
      const std::string& fallback = "") {
    if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
      return fallback;
    }
    return value.at(key).get<std::string>();
  }

  static bool jsonBool(const nlohmann::json& value, const std::string& key, bool fallback = false) {
    if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean()) {
      return fallback;
    }
    return value.at(key).get<bool>();
  }

  static int jsonInt(const nlohmann::json& value, const std::string& key, int fallback = 0) {
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

  static double jsonDouble(const nlohmann::json& value, const std::string& key, double fallback = 0.0) {
    if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
      return fallback;
    }
    const double parsed = value.at(key).get<double>();
    return std::isfinite(parsed) ? parsed : fallback;
  }

  static double normalizeTime(double value) {
    if (!std::isfinite(value)) {
      return 0.0;
    }
    return std::round(value * 1.0e9) / 1.0e9;
  }

  static std::string digestJson(const nlohmann::json& value) {
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

  static nlohmann::json jsonForStrings(const std::vector<std::string>& values) {
    nlohmann::json out = nlohmann::json::array();
    for (const std::string& value : values) {
      out.push_back(value);
    }
    return out;
  }

  static nlohmann::json jsonForDoubles(const std::set<double>& values) {
    nlohmann::json out = nlohmann::json::array();
    for (const double value : values) {
      out.push_back(value);
    }
    return out;
  }

  static std::string rateRelation(double source_period_s, double target_period_s) {
    if (std::abs(source_period_s - target_period_s) <= kEpsilon) {
      return "same_rate";
    }
    if (source_period_s + kEpsilon < target_period_s) {
      return "fast_to_slow";
    }
    return "slow_to_fast";
  }

  std::vector<Row> parseRows() const {
    std::vector<Row> rows;
    const auto dispatch_table = scheduler_table_.value("dispatch_table", nlohmann::json::array());
    if (!dispatch_table.is_array()) {
      return rows;
    }
    int fallback_order = 0;
    for (const auto& item : dispatch_table) {
      if (!item.is_object()) {
        continue;
      }
      Row row;
      row.entry_id = jsonString(item, "entry_id");
      row.node_id = jsonString(item, "node_id");
      if (row.node_id.empty()) {
        continue;
      }
      row.operator_id = jsonString(item, "operator_id");
      row.trigger = jsonString(item, "trigger", "periodic");
      row.capacity_group = jsonString(item, "capacity_group");
      row.resource_lock_mode = jsonString(item, "resource_lock_mode");
      row.can_run_parallel = jsonBool(item, "can_run_parallel", false);
      row.period_s = normalizeTime(jsonDouble(item, "period_s", 0.0));
      row.offset_s = normalizeTime(jsonDouble(item, "offset_s", 0.0));
      row.due_time_s = normalizeTime(jsonDouble(item, "due_time_s", row.offset_s));
      row.timeout_s = jsonDouble(item, "timeout_s", 0.0);
      const double deadline_time_s = jsonDouble(item, "deadline_time_s", 0.0);
      row.deadline_s = row.timeout_s > 0.0 ? row.timeout_s : deadline_time_s;
      row.scheduling_level = jsonInt(item, "scheduling_level", 0);
      row.priority_rank = jsonInt(item, "priority_rank", row.scheduling_level);
      row.dispatch_order = jsonInt(item, "dispatch_order", fallback_order);
      row.source_order = jsonInt(item, "source_order", fallback_order);
      row.depends_on_json = item.value("depends_on", nlohmann::json::array());
      if (row.depends_on_json.is_array()) {
        for (const auto& dep : row.depends_on_json) {
          if (dep.is_string()) {
            row.depends_on.push_back(dep.get<std::string>());
          }
        }
      } else {
        row.depends_on_json = nlohmann::json::array();
      }
      row.input_alignment = item.value("input_alignment", nlohmann::json::array());
      rows.push_back(std::move(row));
      ++fallback_order;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
      if (left.scheduling_level != right.scheduling_level) {
        return left.scheduling_level < right.scheduling_level;
      }
      if (left.priority_rank != right.priority_rank) {
        return left.priority_rank < right.priority_rank;
      }
      if (left.dispatch_order != right.dispatch_order) {
        return left.dispatch_order < right.dispatch_order;
      }
      if (left.source_order != right.source_order) {
        return left.source_order < right.source_order;
      }
      return left.node_id < right.node_id;
    });
    return rows;
  }

  static std::map<std::string, Row> rowsById(const std::vector<Row>& rows) {
    std::map<std::string, Row> out;
    for (const Row& row : rows) {
      out[row.node_id] = row;
    }
    return out;
  }

  nlohmann::json buildRateGraph(
      const std::vector<Row>& rows,
      const std::map<std::string, Row>& row_by_id) const {
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json edges = nlohmann::json::array();
    std::set<double> distinct_periods;
    int dependency_edge_count = 0;
    int transition_edge_count = 0;
    int cross_rate_edge_count = 0;

    for (const Row& row : rows) {
      if (row.period_s > kEpsilon) {
        distinct_periods.insert(row.period_s);
      }
      nodes.push_back({
          {"node_id", row.node_id},
          {"operator_id", row.operator_id},
          {"trigger", row.trigger},
          {"period_s", row.period_s},
          {"offset_s", row.offset_s},
          {"deadline_s", row.deadline_s},
          {"scheduling_level", row.scheduling_level},
          {"priority_rank", row.priority_rank},
          {"capacity_group", row.capacity_group},
          {"resource_lock_mode", row.resource_lock_mode},
          {"can_run_parallel", row.can_run_parallel},
      });
      for (const std::string& dep : row.depends_on) {
        const auto source = row_by_id.find(dep);
        const double source_period_s =
            source == row_by_id.end() ? 0.0 : source->second.period_s;
        const std::string relation = rateRelation(source_period_s, row.period_s);
        if (relation != "same_rate") {
          ++cross_rate_edge_count;
        }
        edges.push_back({
            {"edge_kind", "dependency"},
            {"source_node_id", dep},
            {"target_node_id", row.node_id},
            {"source_period_s", source_period_s},
            {"target_period_s", row.period_s},
            {"rate_relation", relation},
        });
        ++dependency_edge_count;
      }
    }

    const auto transition_table = scheduler_table_.value("transition_table", nlohmann::json::array());
    if (transition_table.is_array()) {
      for (const auto& item : transition_table) {
        if (!item.is_object()) {
          continue;
        }
        const std::string source_node_id = jsonString(item, "source_node_id");
        const std::string target_node_id = jsonString(item, "target_node_id");
        const auto source = row_by_id.find(source_node_id);
        const auto target = row_by_id.find(target_node_id);
        const double source_period_s =
            source == row_by_id.end() ? 0.0 : source->second.period_s;
        const double target_period_s =
            target == row_by_id.end() ? 0.0 : target->second.period_s;
        const std::string relation =
            jsonString(item, "rate_relation", rateRelation(source_period_s, target_period_s));
        if (relation != "same_rate") {
          ++cross_rate_edge_count;
        }
        edges.push_back({
            {"edge_kind", "rate_transition"},
            {"transition_id", jsonString(item, "transition_id")},
            {"binding_id", jsonString(item, "binding_id")},
            {"source_node_id", source_node_id},
            {"target_node_id", target_node_id},
            {"source_port_id", jsonString(item, "source_port_id")},
            {"target_port_id", jsonString(item, "target_port_id")},
            {"source_period_s", source_period_s},
            {"target_period_s", target_period_s},
            {"rate_relation", relation},
            {"strategy", jsonString(item, "strategy")},
            {"requires_runtime_transition", jsonBool(item, "requires_runtime_transition", false)},
            {"insertion_mode", jsonString(item, "insertion_mode")},
            {"transition_node_id", jsonString(item, "transition_node_id")},
        });
        ++transition_edge_count;
      }
    }

    nlohmann::json summary = {
        {"node_count", nodes.size()},
        {"edge_count", edges.size()},
        {"dependency_edge_count", dependency_edge_count},
        {"transition_edge_count", transition_edge_count},
        {"cross_rate_edge_count", cross_rate_edge_count},
        {"distinct_period_count", distinct_periods.size()},
        {"distinct_period_s", jsonForDoubles(distinct_periods)},
    };
    summary["graph_digest_algorithm"] = "fnv64";
    summary["graph_digest"] = digestJson({{"nodes", nodes}, {"edges", edges}});
    return {{"summary", summary}, {"nodes", nodes}, {"edges", edges}};
  }

  nlohmann::json buildMultitasking(const std::map<std::string, Row>& row_by_id) const {
    std::map<int, std::map<int, std::vector<nlohmann::json>>> by_tick_and_level;
    const auto events = due_time_trace_.value("events", nlohmann::json::array());
    if (events.is_array()) {
      for (const auto& event : events) {
        if (!event.is_object() || jsonString(event, "event_kind") != "dispatch") {
          continue;
        }
        by_tick_and_level[jsonInt(event, "tick_index", 0)][jsonInt(event, "scheduling_level", 0)].push_back(event);
      }
    }

    nlohmann::json batches = nlohmann::json::array();
    int batch_count = 0;
    int parallelizable_batch_count = 0;
    int max_batch_width = 0;
    int dependency_violation_count = 0;
    int resource_conflict_count = 0;

    for (const auto& tick_pair : by_tick_and_level) {
      const int tick_index = tick_pair.first;
      int batch_index = 0;
      for (const auto& level_pair : tick_pair.second) {
        std::vector<nlohmann::json> events_in_batch = level_pair.second;
        std::sort(events_in_batch.begin(), events_in_batch.end(), [](const nlohmann::json& left, const nlohmann::json& right) {
          const int left_ordinal = RuntimeScheduleDiagnostics::jsonInt(left, "ordinal_in_tick", 0);
          const int right_ordinal = RuntimeScheduleDiagnostics::jsonInt(right, "ordinal_in_tick", 0);
          if (left_ordinal != right_ordinal) {
            return left_ordinal < right_ordinal;
          }
          return RuntimeScheduleDiagnostics::jsonString(left, "node_id") <
                 RuntimeScheduleDiagnostics::jsonString(right, "node_id");
        });

        std::set<std::string> nodes_in_batch;
        for (const auto& event : events_in_batch) {
          nodes_in_batch.insert(jsonString(event, "node_id"));
        }

        nlohmann::json worker_assignments = nlohmann::json::array();
        std::vector<std::string> node_ids;
        int assignment_index = 0;
        for (const auto& event : events_in_batch) {
          const std::string node_id = jsonString(event, "node_id");
          node_ids.push_back(node_id);
          const auto row = row_by_id.find(node_id);
          if (row != row_by_id.end()) {
            for (const std::string& dep : row->second.depends_on) {
              if (nodes_in_batch.count(dep) > 0) {
                ++dependency_violation_count;
              }
            }
            if (row->second.resource_lock_mode == "exclusive" && events_in_batch.size() > 1) {
              ++resource_conflict_count;
            }
          }
          worker_assignments.push_back({
              {"node_id", node_id},
              {"worker_slot", assignment_index % worker_count_},
              {"ordinal_in_batch", assignment_index},
          });
          ++assignment_index;
        }

        const int width = static_cast<int>(events_in_batch.size());
        max_batch_width = std::max(max_batch_width, width);
        if (width > 1) {
          ++parallelizable_batch_count;
        }
        batches.push_back({
            {"tick_index", tick_index},
            {"tick_time_s", events_in_batch.empty() ? 0.0 : jsonDouble(events_in_batch.front(), "tick_time_s", 0.0)},
            {"batch_index", batch_index++},
            {"scheduling_level", level_pair.first},
            {"node_count", width},
            {"node_ids", jsonForStrings(node_ids)},
            {"worker_assignments", worker_assignments},
            {"deterministic_sort_keys", {"tick_index", "scheduling_level", "ordinal_in_tick", "node_id"}},
        });
        ++batch_count;
      }
    }

    nlohmann::json summary = {
        {"semantic_mode", "deterministic_multitasking_plan"},
        {"runtime_dispatch_mode", "host_serialized_with_deterministic_multitasking_evidence"},
        {"enabled", worker_count_ > 1},
        {"worker_count", worker_count_},
        {"batch_count", batch_count},
        {"parallelizable_batch_count", parallelizable_batch_count},
        {"max_batch_width", max_batch_width},
        {"dependency_violation_count", dependency_violation_count},
        {"resource_conflict_count", resource_conflict_count},
        {"deterministic_order", dependency_violation_count == 0},
    };
    summary["multitasking_digest_algorithm"] = "fnv64";
    summary["multitasking_digest"] = digestJson({{"summary", summary}, {"batches", batches}});
    return {{"summary", summary}, {"batches", batches}};
  }

  nlohmann::json buildTiming(const std::map<std::string, Row>& row_by_id) const {
    nlohmann::json checks = nlohmann::json::array();
    std::map<std::string, double> previous_dispatch_time;
    int deadline_check_count = 0;
    int deadline_miss_count = 0;
    int overrun_count = 0;
    int jitter_violation_count = 0;
    double max_abs_jitter_s = 0.0;

    const auto events = due_time_trace_.value("events", nlohmann::json::array());
    if (events.is_array()) {
      for (const auto& event : events) {
        if (!event.is_object() || jsonString(event, "event_kind") != "dispatch") {
          continue;
        }
        const std::string node_id = jsonString(event, "node_id");
        const auto row = row_by_id.find(node_id);
        const double period_s = row == row_by_id.end() ? jsonDouble(event, "period_s", 0.0) : row->second.period_s;
        const double deadline_s = row == row_by_id.end() ? 0.0 : row->second.deadline_s;
        const double tick_time_s = jsonDouble(event, "tick_time_s", 0.0);
        const double due_time_s = jsonDouble(event, "due_time_s", tick_time_s);
        const double lateness_s = std::max(0.0, normalizeTime(tick_time_s - due_time_s));
        double jitter_s = 0.0;
        bool has_previous = false;
        const auto previous = previous_dispatch_time.find(node_id);
        if (previous != previous_dispatch_time.end() && period_s > kEpsilon) {
          has_previous = true;
          jitter_s = normalizeTime((tick_time_s - previous->second) - period_s);
          max_abs_jitter_s = std::max(max_abs_jitter_s, std::abs(jitter_s));
          if (std::abs(jitter_s) > 1.0e-7) {
            ++jitter_violation_count;
          }
        }
        previous_dispatch_time[node_id] = tick_time_s;

        const bool deadline_configured = deadline_s > kEpsilon;
        const bool deadline_missed = deadline_configured && lateness_s > deadline_s + kEpsilon;
        if (deadline_missed) {
          ++deadline_miss_count;
        }
        const double assumed_duration_s = 0.0;
        const double overrun_s = deadline_configured
                                     ? std::max(0.0, assumed_duration_s - deadline_s)
                                     : 0.0;
        if (overrun_s > kEpsilon) {
          ++overrun_count;
        }

        checks.push_back({
            {"node_id", node_id},
            {"tick_index", jsonInt(event, "tick_index", 0)},
            {"tick_time_s", tick_time_s},
            {"due_time_s", due_time_s},
            {"period_s", period_s},
            {"deadline_s", deadline_s},
            {"deadline_status", deadline_missed ? "missed" : (deadline_configured ? "within_deadline" : "not_configured")},
            {"lateness_s", lateness_s},
            {"overrun_s", overrun_s},
            {"jitter_s", jitter_s},
            {"has_previous_dispatch", has_previous},
            {"estimated_duration_source", "not_measured_in_diagnostic_trace"},
        });
        ++deadline_check_count;
      }
    }

    nlohmann::json summary = {
        {"deadline_check_event_count", deadline_check_count},
        {"deadline_miss_count", deadline_miss_count},
        {"overrun_count", overrun_count},
        {"jitter_violation_count", jitter_violation_count},
        {"max_abs_jitter_s", max_abs_jitter_s},
        {"jitter_tolerance_s", 1.0e-7},
        {"duration_measurement_mode", "diagnostic_trace_only"},
    };
    summary["timing_digest_algorithm"] = "fnv64";
    summary["timing_digest"] = digestJson({{"summary", summary}, {"checks", checks}});
    return {{"summary", summary}, {"checks", checks}};
  }

  nlohmann::json scheduler_table_ = nlohmann::json::object();
  nlohmann::json due_time_trace_ = nlohmann::json::object();
  int worker_count_ = 1;
};

}  // namespace FlightEnvPlatformRuntime
