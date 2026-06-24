/**
 * @file RuntimeProfileUtils.cpp
 * @brief 实现 runtime profile 通用读取工具。
 *
 * 大概：把 PlatformRuntimeHost 中原本容易继续膨胀的 profile 解释逻辑拆出来。
 * 具体：提供终止 stop_reason 收集和 profile 声明指标抽取；不识别任何对象专属字段含义。
 * 被谁使用：被 runtime host 的分支收口、health ledger 和摘要生成逻辑使用。
 * 使用谁：使用 nlohmann::json 和标准库，不调用对象算子。
 */

#include "FlightEnvPlatformRuntime/RuntimeProfileUtils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace FlightEnvPlatformRuntime {
namespace {

std::string jsonString(const nlohmann::json& value,
                       const std::string& key,
                       const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

bool jsonBool(const nlohmann::json& value, const std::string& key, bool fallback = false) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean()) {
    return fallback;
  }
  return value.at(key).get<bool>();
}

nlohmann::json objectOrEmpty(const nlohmann::json& value) {
  return value.is_object() ? value : nlohmann::json::object();
}

std::vector<std::string> jsonStringArray(const nlohmann::json& value, const std::string& key) {
  std::vector<std::string> out;
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_array()) {
    return out;
  }
  for (const auto& item : value.at(key)) {
    if (item.is_string()) {
      out.push_back(item.get<std::string>());
    }
  }
  return out;
}

void appendUnique(std::vector<std::string>& target, const std::string& value) {
  if (value.empty()) {
    return;
  }
  if (std::find(target.begin(), target.end(), value) == target.end()) {
    target.push_back(value);
  }
}

double findNumberRecursive(const nlohmann::json& value,
                           const std::vector<std::string>& keys,
                           int depth = 0) {
  if (depth > 8) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (value.is_object()) {
    for (const std::string& key : keys) {
      if (value.contains(key) && value.at(key).is_number()) {
        return value.at(key).get<double>();
      }
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
      const double found = findNumberRecursive(it.value(), keys, depth + 1);
      if (std::isfinite(found)) {
        return found;
      }
    }
  }
  if (value.is_array()) {
    for (const auto& item : value) {
      const double found = findNumberRecursive(item, keys, depth + 1);
      if (std::isfinite(found)) {
        return found;
      }
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

std::vector<std::string> metricKeysForId(const nlohmann::json& profile, const std::string& metric_id) {
  std::vector<std::string> keys;
  const nlohmann::json profile_object = objectOrEmpty(profile);
  const nlohmann::json health_ledger = objectOrEmpty(profile_object.value("health_ledger", nlohmann::json::object()));
  const nlohmann::json health_keys = objectOrEmpty(health_ledger.value("metric_keys", nlohmann::json::object()));
  for (const std::string& key : jsonStringArray(health_keys, metric_id)) {
    appendUnique(keys, key);
  }

  const nlohmann::json termination =
      objectOrEmpty(profile_object.value("termination_policy", nlohmann::json::object()));
  const nlohmann::json termination_keys =
      objectOrEmpty(termination.value("metric_key_groups", nlohmann::json::object()));
  for (const std::string& key : jsonStringArray(termination_keys, metric_id)) {
    appendUnique(keys, key);
  }

  appendUnique(keys, metric_id);
  return keys;
}

double findMetricByKeys(const nlohmann::json& primary,
                        const nlohmann::json& secondary,
                        const std::vector<std::string>& keys) {
  if (primary.is_object()) {
    for (const std::string& key : keys) {
      if (primary.contains(key) && primary.at(key).is_number()) {
        return primary.at(key).get<double>();
      }
    }
  }
  return findNumberRecursive(secondary, keys);
}

}  // namespace

std::vector<std::string> terminalStopReasonsFromProfile(const nlohmann::json& profile) {
  std::vector<std::string> reasons;
  const nlohmann::json policy =
      objectOrEmpty(objectOrEmpty(profile).value("termination_policy", nlohmann::json::object()));
  for (const std::string& reason : jsonStringArray(policy, "terminal_stop_reasons")) {
    appendUnique(reasons, reason);
  }
  if (!reasons.empty()) {
    return reasons;
  }

  const nlohmann::json rules = objectOrEmpty(policy.value("alternative_rules", nlohmann::json::object()));
  if (rules.is_object()) {
    for (auto it = rules.begin(); it != rules.end(); ++it) {
      if (it.value().is_object()) {
        appendUnique(reasons, jsonString(it.value(), "stop_reason"));
      }
    }
  }
  return reasons;
}

bool stopReasonMatchesTerminalPolicy(const std::string& stop_reason, const nlohmann::json& profile) {
  const std::vector<std::string> reasons = terminalStopReasonsFromProfile(profile);
  return std::find(reasons.begin(), reasons.end(), stop_reason) != reasons.end();
}

nlohmann::json collectConfiguredMetrics(const nlohmann::json& primary,
                                        const nlohmann::json& secondary,
                                        const nlohmann::json& profile,
                                        const std::string& section_key) {
  const nlohmann::json health_ledger =
      objectOrEmpty(objectOrEmpty(profile).value("health_ledger", nlohmann::json::object()));
  const nlohmann::json metric_specs =
      objectOrEmpty(health_ledger).value(section_key, nlohmann::json::array());
  nlohmann::json out = nlohmann::json::object();
  if (!metric_specs.is_array()) {
    return out;
  }

  for (const auto& spec : metric_specs) {
    if (!spec.is_object()) {
      continue;
    }
    const std::string metric_id = jsonString(spec,
                                             "metric_id",
                                             jsonString(spec, "metric_key_group", jsonString(spec, "metric_name")));
    const std::string output_key = jsonString(spec, "output_key", metric_id);
    if (output_key.empty()) {
      continue;
    }

    std::vector<std::string> keys = jsonStringArray(spec, "keys");
    if (keys.empty() && !metric_id.empty()) {
      keys = metricKeysForId(profile, metric_id);
    }
    const double value = keys.empty()
                             ? std::numeric_limits<double>::quiet_NaN()
                             : findMetricByKeys(primary, secondary, keys);
    if (std::isfinite(value)) {
      out[output_key] = value;
    } else if (jsonBool(spec, "emit_null", false)) {
      out[output_key] = nullptr;
    }
  }
  return out;
}

}  // namespace FlightEnvPlatformRuntime
