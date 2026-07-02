#pragma once

/**
 * @file RuntimeDueTimeScheduler.hpp
 * @brief Builds deterministic due-time scheduler traces from scheduler_table.json.
 *
 * This component is intentionally narrow: it turns the Phase6 scheduler table
 * contract into an auditable single-thread dispatch trace. It does not encode
 * object semantics or operator behavior.
 */

#include "FlightEnvPlatformRuntime/time/RuntimeTimeTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
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

class RuntimeDueTimeScheduler {
 public:
  explicit RuntimeDueTimeScheduler(nlohmann::json scheduler_table)
      : scheduler_table_(std::move(scheduler_table)) {}

  nlohmann::json buildTrace() const {
    const std::vector<Row> rows = parseRows();
    const double quantum_s = dispatchQuantumS(rows);
    const double horizon_s = dispatchHorizonS(rows, quantum_s);
    const int max_tick_count = 512;
    // 直接枚举各行的绝对到期时刻 (first_due + k*period) 作为派发栅格，与 live 种子
    // seedWorkflowEvents 的绝对栅格语义一致。旧实现以最小周期为均匀量子扫描时间轴，
    // 会漏掉与最小周期不可整除的到期点（如 2ms/3ms 两行时，3ms 行只在 6ms、12ms
    // 命中，3ms、9ms 被跳过），导致审计基准本身漏拍。quantum_s 仍保留为最小周期，
    // 仅用于 summary 与慢节点分类。
    std::vector<double> tick_times;
    if (quantum_s > kEpsilon) {
      std::set<double> due_grid;
      for (const Row& row : rows) {
        if (!(row.period_s > kEpsilon)) {
          continue;
        }
        // 每行至多贡献 max_tick_count 个最早到期点：全局最早 512 个去重时刻
        // 必然包含于各行前 512 个到期点的并集，因此该上限不影响截断语义。
        for (int due_index = 0; due_index < max_tick_count; ++due_index) {
          const double due_time_s = normalizeTime(
              row.first_due_time_s + static_cast<double>(due_index) * row.period_s);
          if (due_time_s > horizon_s + kEpsilon) {
            break;
          }
          due_grid.insert(due_time_s);
        }
      }
      tick_times.assign(due_grid.begin(), due_grid.end());
      if (static_cast<int>(tick_times.size()) > max_tick_count) {
        tick_times.resize(static_cast<std::size_t>(max_tick_count));
      }
    }
    const int tick_count = static_cast<int>(tick_times.size());

    nlohmann::json events = nlohmann::json::array();
    std::set<double> distinct_due_times;
    int dispatch_count = 0;
    int held_count = 0;
    int slow_held_count = 0;
    int not_due_violation_count = 0;
    int dependency_violation_count = 0;

    for (int tick_index = 0; tick_index < tick_count; ++tick_index) {
      const double tick_time_s = tick_times[static_cast<std::size_t>(tick_index)];
      std::vector<Row> due_rows;
      std::set<std::string> due_node_ids;
      for (const Row& row : rows) {
        if (isDueAt(row, tick_time_s)) {
          due_rows.push_back(row);
          due_node_ids.insert(row.node_id);
        } else {
          const double next_due_time_s = nextDueTimeS(row, tick_time_s);
          const double last_due_time_s = lastDueTimeS(row, tick_time_s);
          events.push_back({
              {"event_kind", "held_not_due"},
              {"tick_index", tick_index},
              {"tick_time_s", tick_time_s},
              {"node_id", row.node_id},
              {"entry_id", row.entry_id},
              {"period_s", row.period_s},
              {"offset_s", row.offset_s},
              {"last_due_time_s", last_due_time_s},
              {"next_due_time_s", next_due_time_s},
              {"dispatch_order", row.dispatch_order},
              {"scheduling_level", row.scheduling_level},
              {"priority_rank", row.priority_rank},
              {"capacity_group", row.capacity_group},
              {"resource_lock_mode", row.resource_lock_mode},
          });
          ++held_count;
          if (row.period_s > quantum_s + kEpsilon) {
            ++slow_held_count;
          }
        }
      }

      std::sort(due_rows.begin(), due_rows.end(), [](const Row& left, const Row& right) {
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

      std::set<std::string> dispatched_this_tick;
      int ordinal_in_tick = 0;
      for (const Row& row : due_rows) {
        if (!isDueAt(row, tick_time_s)) {
          ++not_due_violation_count;
        }
        bool dependency_ready = true;
        nlohmann::json unmet_same_tick_dependencies = nlohmann::json::array();
        for (const std::string& dep : row.depends_on) {
          if (due_node_ids.count(dep) == 0) {
            continue;
          }
          if (dispatched_this_tick.count(dep) == 0) {
            dependency_ready = false;
            unmet_same_tick_dependencies.push_back(dep);
          }
        }
        if (!dependency_ready) {
          ++dependency_violation_count;
        }
        dispatched_this_tick.insert(row.node_id);
        distinct_due_times.insert(tick_time_s);
        events.push_back({
            {"event_kind", "dispatch"},
            {"tick_index", tick_index},
            {"tick_time_s", tick_time_s},
            {"due_time_s", tick_time_s},
            {"ordinal_in_tick", ordinal_in_tick++},
            {"node_id", row.node_id},
            {"entry_id", row.entry_id},
            {"operator_id", row.operator_id},
            {"trigger", row.trigger},
            {"period_s", row.period_s},
            {"offset_s", row.offset_s},
            {"dispatch_order", row.dispatch_order},
            {"source_order", row.source_order},
            {"scheduling_level", row.scheduling_level},
            {"priority_rank", row.priority_rank},
            {"capacity_group", row.capacity_group},
            {"resource_lock_mode", row.resource_lock_mode},
            {"can_run_parallel_source", row.can_run_parallel_source},
            {"single_thread_dispatch", true},
            {"dependency_ready", dependency_ready},
            {"unmet_same_tick_dependencies", unmet_same_tick_dependencies},
            {"depends_on", row.depends_on_json},
            {"input_alignment", row.input_alignment},
        });
        ++dispatch_count;
      }
    }

    nlohmann::json distinct_due_time_values = nlohmann::json::array();
    for (const double value : distinct_due_times) {
      distinct_due_time_values.push_back(value);
    }

    nlohmann::json stable_summary = {
        {"row_count", rows.size()},
        {"dispatch_event_count", dispatch_count},
        {"held_not_due_event_count", held_count},
        {"slow_node_held_event_count", slow_held_count},
        {"not_due_violation_count", not_due_violation_count},
        {"dependency_violation_count", dependency_violation_count},
        {"distinct_due_time_count", distinct_due_times.size()},
        {"distinct_due_time_s", distinct_due_time_values},
        {"quantum_s", quantum_s},
        {"horizon_s", horizon_s},
        {"tick_count", tick_count},
        {"deterministic_single_thread", true},
        {"expanded_from_scheduler_table", true},
        {"tick_enumeration", "absolute_due_times"},
        {"first_due_rule", "offset_plus_period_or_public_tick_dependency_floor"},
        {"dispatch_sort_keys", {"scheduling_level", "priority_rank", "dispatch_order", "source_order", "node_id"}},
    };
    const nlohmann::json stable_payload = {{"summary", stable_summary}, {"events", events}};
    stable_summary["trace_digest_algorithm"] = "fnv64";
    stable_summary["trace_digest"] = digestJson(stable_payload);

    return {
        {"schema_version", "flightenv.platform.scheduler_due_time_trace.v1"},
        {"source_scheduler_table_schema", scheduler_table_.value("schema_version", std::string())},
        {"source_table_id", scheduler_table_.value("table_id", std::string())},
        {"workflow_id", scheduler_table_.value("workflow_id", std::string())},
        {"object_id", scheduler_table_.value("object_id", std::string())},
        {"phase", scheduler_table_.value("phase", std::string())},
        {"solver_policy", scheduler_table_.value("solver_policy", nlohmann::json::object())},
        {"summary", stable_summary},
        {"events", events},
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
    bool can_run_parallel_source = false;
    double period_s = 0.0;
    double offset_s = 0.0;
    double first_due_time_s = 0.0;
    int dispatch_order = 0;
    int source_order = 0;
    int scheduling_level = 0;
    int priority_rank = 0;
    std::vector<std::string> depends_on;
    nlohmann::json depends_on_json = nlohmann::json::array();
    nlohmann::json input_alignment = nlohmann::json::array();
  };

  static constexpr double kEpsilon = 1.0e-9;

  static double jsonDouble(const nlohmann::json& value, const std::string& key, double fallback = 0.0) {
    if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
      return fallback;
    }
    return value.at(key).get<double>();
  }

  static int jsonInt(const nlohmann::json& value, const std::string& key, int fallback = 0) {
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

  std::vector<Row> parseRows() const {
    std::vector<Row> rows;
    const double base_dt_s = jsonDouble(
        scheduler_table_.value("solver_policy", nlohmann::json::object()),
        "base_dt_s",
        1.0);
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
      row.can_run_parallel_source = jsonBool(item, "can_run_parallel", false);
      row.period_s = normalizeTime(jsonDouble(item, "period_s", base_dt_s));
      if (!(row.period_s > kEpsilon)) {
        row.period_s = normalizeTime(base_dt_s > kEpsilon ? base_dt_s : 1.0);
      }
      row.offset_s = normalizeTime(jsonDouble(item, "offset_s", 0.0));
      // 首拍规则与 live 种子同源（runtimeNodeFirstDueNanoseconds）：快节点 = offset + period，
      // 慢节点 = offset + 公共拍。有意不再使用表内 due_time_s —— 它取自 time_point.run_time_s
      // 的区间起点约定（多为 0），与 runtime 区间末端输出约定相差一拍，正是旧审计 trace
      // 与实际执行相位分歧的根源。
      {
        const std::int64_t period_ns = RuntimeDuration::fromSeconds(row.period_s).nanoseconds;
        const std::int64_t public_ns =
            RuntimeDuration::fromSeconds(base_dt_s > kEpsilon ? base_dt_s : row.period_s).nanoseconds;
        const std::int64_t offset_ns = RuntimeDuration::fromSeconds(row.offset_s).nanoseconds;
        row.first_due_time_s = normalizeTime(
            RuntimeTimePoint::fromNanoseconds(
                runtimeNodeFirstDueNanoseconds(period_ns, public_ns, offset_ns))
                .seconds());
      }
      row.dispatch_order = jsonInt(item, "dispatch_order", fallback_order);
      row.source_order = jsonInt(item, "source_order", fallback_order);
      row.scheduling_level = jsonInt(item, "scheduling_level", 0);
      row.priority_rank = jsonInt(item, "priority_rank", row.scheduling_level);
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
      if (left.dispatch_order != right.dispatch_order) {
        return left.dispatch_order < right.dispatch_order;
      }
      if (left.source_order != right.source_order) {
        return left.source_order < right.source_order;
      }
      return left.node_id < right.node_id;
    });
    applyDependencyAwareFirstDue(rows);
    return rows;
  }

  static void applyDependencyAwareFirstDue(std::vector<Row>& rows) {
    std::map<std::string, std::size_t> index_by_node;
    for (std::size_t index = 0; index < rows.size(); ++index) {
      if (!rows[index].node_id.empty()) {
        index_by_node[rows[index].node_id] = index;
      }
    }

    std::map<std::string, std::int64_t> resolved_first_due_ns_by_node;
    std::set<std::string> resolving;
    std::function<std::int64_t(const std::string&)> resolveFirstDueNs =
        [&](const std::string& node_id) -> std::int64_t {
      const auto existing = resolved_first_due_ns_by_node.find(node_id);
      if (existing != resolved_first_due_ns_by_node.end()) {
        return existing->second;
      }
      const auto index_found = index_by_node.find(node_id);
      if (index_found == index_by_node.end()) {
        return 0;
      }
      Row& row = rows[index_found->second];
      std::int64_t resolved_ns = RuntimeDuration::fromSeconds(row.first_due_time_s).nanoseconds;
      if (resolving.count(node_id) != 0) {
        return resolved_ns;
      }
      resolving.insert(node_id);
      for (const std::string& dep : row.depends_on) {
        if (index_by_node.count(dep) == 0) {
          continue;
        }
        resolved_ns = runtimeNodeFirstDueAfterDependencyNanoseconds(
            resolved_ns,
            resolveFirstDueNs(dep));
      }
      resolving.erase(node_id);
      resolved_first_due_ns_by_node[node_id] = resolved_ns;
      return resolved_ns;
    };

    for (Row& row : rows) {
      row.first_due_time_s =
          normalizeTime(RuntimeTimePoint::fromNanoseconds(resolveFirstDueNs(row.node_id)).seconds());
    }
  }

  static double dispatchQuantumS(const std::vector<Row>& rows) {
    double quantum_s = std::numeric_limits<double>::infinity();
    for (const Row& row : rows) {
      if (row.period_s > kEpsilon && row.period_s < quantum_s) {
        quantum_s = row.period_s;
      }
    }
    if (!std::isfinite(quantum_s) || !(quantum_s > kEpsilon)) {
      return 0.0;
    }
    return normalizeTime(quantum_s);
  }

  static double dispatchHorizonS(const std::vector<Row>& rows, double quantum_s) {
    double horizon_s = quantum_s;
    for (const Row& row : rows) {
      horizon_s = std::max(horizon_s, row.first_due_time_s + row.period_s);
    }
    return normalizeTime(std::max(horizon_s, quantum_s));
  }

  static bool isDueAt(const Row& row, double tick_time_s) {
    if (!(row.period_s > kEpsilon)) {
      return false;
    }
    if (tick_time_s + kEpsilon < row.first_due_time_s) {
      return false;
    }
    const double scaled = (tick_time_s - row.first_due_time_s) / row.period_s;
    const double nearest = std::round(scaled);
    return std::abs(scaled - nearest) <= 1.0e-7;
  }

  static double nextDueTimeS(const Row& row, double tick_time_s) {
    if (!(row.period_s > kEpsilon)) {
      return row.first_due_time_s;
    }
    if (tick_time_s + kEpsilon < row.first_due_time_s) {
      return row.first_due_time_s;
    }
    const double scaled = (tick_time_s - row.first_due_time_s) / row.period_s;
    return normalizeTime(row.first_due_time_s + (std::floor(scaled) + 1.0) * row.period_s);
  }

  static double lastDueTimeS(const Row& row, double tick_time_s) {
    if (!(row.period_s > kEpsilon) || tick_time_s + kEpsilon < row.first_due_time_s) {
      return -1.0;
    }
    const double scaled = (tick_time_s - row.first_due_time_s) / row.period_s;
    return normalizeTime(row.first_due_time_s + std::floor(scaled) * row.period_s);
  }

  nlohmann::json scheduler_table_ = nlohmann::json::object();
};

}  // namespace FlightEnvPlatformRuntime
