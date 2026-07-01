/**
 * @file PlatformRuntimeHost.cpp
 * @brief 实现平台运行宿主门面。
 *
 * 大概：这是上层程序实际调用的平台 runtime 外壳。
 * 具体：它把加载、启动、停止、推进、分支查询和 evidence 聚合包装成稳定入口。
 * 被谁使用：被 runtime exe、launcher、SDK、UI 集成层和端到端测试使用。
 * 使用谁：使用 NativeWorkflowRunner、RuntimeTimeScheduler、PDK runtime/evidence 类型。
 * 拆分判断：能总结但偏门面聚合；新增复杂分支或证据逻辑时应拆成 BranchService/EvidenceService。
 */

#include "FlightEnvPlatformRuntime/PlatformRuntimeHost.hpp"

#include "FlightEnvPlatformRuntime/NativeWorkflowRunner.hpp"
#include "FlightEnvPlatformRuntime/RuntimeBranchService.hpp"
#include "FlightEnvPlatformRuntime/RuntimeProfileUtils.hpp"
#include "FlightEnvPlatformRuntime/RuntimeTimelineMaterializer.hpp"
#include "FlightEnvPlatformRuntime/RuntimeTypedBufferStore.hpp"
#include "FlightEnvPlatformRuntime/RuntimeZeroCopyPolicy.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeEventQueue.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace FlightEnvPlatformRuntime {
namespace {

std::string nowUtcIso() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const std::time_t time = clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

std::string pathString(const fs::path& path) {
  return path.lexically_normal().string();
}

std::string quoteArg(const std::string& text) {
  std::string escaped;
  escaped.reserve(text.size() + 2);
  escaped.push_back('"');
  for (char c : text) {
    if (c == '"') {
      escaped += "\\\"";
    } else {
      escaped.push_back(c);
    }
  }
  escaped.push_back('"');
  return escaped;
}

std::string quotePsSingle(const std::string& text) {
  std::string escaped;
  escaped.reserve(text.size() + 2);
  escaped.push_back('\'');
  for (char c : text) {
    if (c == '\'') {
      escaped += "''";
    } else {
      escaped.push_back(c);
    }
  }
  escaped.push_back('\'');
  return escaped;
}

json readJson(const fs::path& path) {
  std::string last_error;
  for (int attempt = 0; attempt < 20; ++attempt) {
    try {
      std::ifstream in(path);
      if (!in) {
        last_error = "JSON file not found";
      } else {
        json value;
        in >> value;
        return value;
      }
    } catch (const json::parse_error& exc) {
      last_error = exc.what();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error("Cannot read stable JSON file: " + pathString(path) + " (" + last_error + ")");
}

json readJsonIfExists(const fs::path& path) {
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (fs::exists(path)) {
      return readJson(path);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return json::object();
}

void writeJson(const fs::path& path, const json& value) {
  if (!path.parent_path().empty()) {
    fs::create_directories(path.parent_path());
  }
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  fs::path tmp = path;
  tmp += ".tmp." + std::to_string(tick);
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) {
      throw std::runtime_error("Cannot write JSON file: " + pathString(tmp));
    }
    out << value.dump(2, ' ', false, json::error_handler_t::replace);
    out << '\n';
  }
  // UI(LiveDataHub)每帧轮询读取这些 evidence 文件；Windows 上读句柄会让替换短暂失败。
  // UI 读窗口是毫秒级，这里重试 + 退避即可越过，不应让整条主链因瞬时文件锁而崩溃。
  constexpr int kReplaceAttempts = 200;  // 200 * 5ms = 最多 ~1s
#ifdef _WIN32
  bool replaced = false;
  for (int attempt = 0; attempt < kReplaceAttempts; ++attempt) {
    if (MoveFileExW(tmp.wstring().c_str(),
                    path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      replaced = true;
      break;
    }
    ::Sleep(5);
  }
  if (!replaced) {
    std::error_code ec;
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) {
      fs::remove(tmp, ec);
      throw std::runtime_error("Cannot replace JSON file: " + pathString(path));
    }
  }
#else
  std::error_code ec;
  bool replaced = false;
  for (int attempt = 0; attempt < kReplaceAttempts; ++attempt) {
    fs::rename(tmp, path, ec);
    if (!ec) {
      replaced = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!replaced) {
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) {
      fs::remove(tmp, ec);
      throw std::runtime_error("Cannot replace JSON file: " + pathString(path));
    }
  }
#endif
}

std::string jsonString(const json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

int jsonInt(const json& value, const std::string& key, int fallback = 0) {
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

double jsonDouble(const json& value, const std::string& key, double fallback = 0.0) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
    return fallback;
  }
  return value.at(key).get<double>();
}

bool jsonBool(const json& value, const std::string& key, bool fallback = false) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean()) {
    return fallback;
  }
  return value.at(key).get<bool>();
}

json objectOrEmpty(const json& value) {
  return value.is_object() ? value : json::object();
}

json arrayOrEmpty(const json& value) {
  return value.is_array() ? value : json::array();
}

std::vector<std::string> jsonStringArray(const json& value) {
  std::vector<std::string> out;
  if (!value.is_array()) {
    return out;
  }
  for (const auto& item : value) {
    if (item.is_string()) {
      out.push_back(item.get<std::string>());
    }
  }
  return out;
}

std::vector<std::string> jsonStringArray(const json& value, const std::string& key) {
  if (!value.is_object() || !value.contains(key)) {
    return {};
  }
  return jsonStringArray(value.at(key));
}

bool stringListContains(const std::vector<std::string>& values, const std::string& needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

bool containsAnyToken(const std::string& text, const std::vector<std::string>& tokens) {
  for (const std::string& token : tokens) {
    if (!token.empty() && text.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

fs::path resolveObjectProfilePath(const fs::path& workspace_root,
                                  const fs::path& object_package_root,
                                  const std::string& raw) {
  fs::path path(raw);
  if (raw.empty()) {
    return {};
  }
  if (path.is_absolute()) {
    return path;
  }
  const bool workspace_relative =
      raw.rfind("_local_artifacts", 0) == 0 || raw.rfind("_deps", 0) == 0;
  return workspace_relative ? workspace_root / path : object_package_root / path;
}

std::string stableIdPathSegment(std::string value) {
  for (char& c : value) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (!(std::isalnum(uc) || c == '.' || c == '_' || c == '-')) {
      c = '_';
    }
  }
  while (!value.empty() && value.front() == '_') {
    value.erase(value.begin());
  }
  while (!value.empty() && value.back() == '_') {
    value.pop_back();
  }
  return value.empty() ? std::string("unnamed") : value;
}

json loadObjectRuntimeProfile(const fs::path& object_package_root) {
  const fs::path manifest_path = object_package_root / "object" / "twin_object.json";
  const json manifest = readJsonIfExists(manifest_path);
  std::string profile_ref;
  if (manifest.is_object()) {
    profile_ref = jsonString(manifest, "platform_runtime_profile");
    if (profile_ref.empty() && manifest.contains("platform_runtime") &&
        manifest.at("platform_runtime").is_object()) {
      profile_ref = jsonString(manifest.at("platform_runtime"), "profile_path");
    }
  }
  fs::path profile_path = profile_ref.empty()
                              ? object_package_root / "runtime" / "platform_runtime_profile.json"
                              : (manifest_path.parent_path() / profile_ref);
  profile_path = profile_path.lexically_normal();
  if (!fs::exists(profile_path)) {
    return json::object();
  }
  json profile = readJson(profile_path);
  profile["_profile_path"] = pathString(profile_path);
  return profile;
}

json workflowRole(const json& profile, const std::string& role) {
  const json roles = objectOrEmpty(profile).value("workflow_roles", json::array());
  if (!roles.is_array()) {
    return json::object();
  }
  for (const auto& item : roles) {
    if (item.is_object() && jsonString(item, "role") == role) {
      return item;
    }
  }
  return json::object();
}

json workflowRoleForWorkflowId(const json& profile, const std::string& workflow_id) {
  const json roles = objectOrEmpty(profile).value("workflow_roles", json::array());
  if (!roles.is_array()) {
    return json::object();
  }
  for (const auto& item : roles) {
    if (item.is_object() && jsonString(item, "workflow_id") == workflow_id) {
      return item;
    }
  }
  return json::object();
}

std::string defaultWorkflowIdForRole(const json& profile, const std::string& role) {
  const json role_item = workflowRole(profile, role);
  if (!role_item.empty()) {
    return jsonString(role_item, "workflow_id");
  }
  const json defaults = objectOrEmpty(profile).value("default_runtime_host", json::object());
  if (role == "online_mainline") {
    return jsonString(defaults, "online_workflow_id");
  }
  if (role == "future_prediction") {
    return jsonString(defaults, "future_workflow_id");
  }
  return "";
}

fs::path compiledWorkflowPathForRole(const json& profile,
                                     const fs::path& workspace_root,
                                     const std::string& role) {
  const std::string workflow_id = defaultWorkflowIdForRole(profile, role);
  if (workflow_id.empty()) {
    return {};
  }
  const json defaults = objectOrEmpty(profile).value("default_runtime_host", json::object());
  const std::string root_ref =
      jsonString(defaults, "compiled_workflow_root", "_local_artifacts/platform-pdk/compiled-workflows");
  fs::path root(root_ref);
  if (!root.is_absolute()) {
    root = workspace_root / root;
  }
  return root / stableIdPathSegment(workflow_id);
}

fs::path firstExistingProfilePath(const json& profile,
                                  const fs::path& workspace_root,
                                  const fs::path& object_package_root,
                                  const std::string& key) {
  for (const std::string& item : jsonStringArray(profile, key)) {
    const fs::path candidate =
        resolveObjectProfilePath(workspace_root, object_package_root, item);
    if (!candidate.empty() && fs::exists(candidate)) {
      return candidate;
    }
  }
  return {};
}

json workflowFromSnapshot(const json& snapshot) {
  if (snapshot.is_object() && snapshot.contains("workflow") && snapshot.at("workflow").is_object()) {
    return snapshot.at("workflow");
  }
  return json::object();
}

int solverMaxSteps(const json& workflow, int fallback) {
  const json solver = workflow.value("solver_policy", json::object());
  const int max_steps = jsonInt(solver, "max_steps", 0);
  return max_steps > 0 ? max_steps : fallback;
}

std::string safeFileName(std::string value) {
  for (char& c : value) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (!(std::isalnum(uc) || c == '.' || c == '_' || c == '-')) {
      c = '_';
    }
  }
  return value.empty() ? std::string("branch") : value;
}

fs::path branchControlPath(const fs::path& chain_dir, const std::string& branch_id) {
  return chain_dir / "branch_controls" / (safeFileName(branch_id) + ".json");
}

fs::path branchManagerStatePath(const fs::path& chain_dir, const std::string& branch_id) {
  return chain_dir / "branch_manager" / (safeFileName(branch_id) + ".json");
}

json lastLoopIteration(const json& loop) {
  if (loop.contains("iterations") && loop.at("iterations").is_array() &&
      !loop.at("iterations").empty()) {
    return loop.at("iterations").back();
  }
  return json::object();
}

std::string branchStopReasonFromLoop(const json& loop) {
  const json last = lastLoopIteration(loop);
  return jsonString(last, "stop_reason", "");
}

bool startsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

constexpr const char* kOnlineFilterBranchId = "main.online";
constexpr const char* kRealtimePredictionBranchId = "main.realtime_prediction";

bool fieldDisplayRoleMatches(const json& item, const json& rule) {
  if (!item.is_object() || !rule.is_object()) {
    return false;
  }
  const std::string field = jsonString(item, "field_name", "");
  const std::string node = jsonString(item, "node_id");
  const std::string port = jsonString(item, "port_id");
  const std::string contract = jsonString(item, "contract_id", "");
  if (stringListContains(jsonStringArray(rule, "field_names"), field)) {
    return true;
  }
  if (stringListContains(jsonStringArray(rule, "node_ids"), node)) {
    return true;
  }
  if (stringListContains(jsonStringArray(rule, "port_ids"), port)) {
    return true;
  }
  if (containsAnyToken(port, jsonStringArray(rule, "port_contains"))) {
    return true;
  }
  if (containsAnyToken(contract, jsonStringArray(rule, "contract_contains"))) {
    return true;
  }
  return jsonBool(rule, "field_name_present", false) && !field.empty();
}

json fieldDisplayRoleRule(const json& profile, const std::string& display_role) {
  const json rules = objectOrEmpty(profile).value("field_display_roles", json::array());
  if (!rules.is_array()) {
    return json::object();
  }
  for (const auto& rule : rules) {
    if (rule.is_object() && jsonString(rule, "display_role") == display_role) {
      return rule;
    }
  }
  return json::object();
}

bool isRealtimePredictionFieldArtifact(const json& item, const json& profile) {
  const json rule = fieldDisplayRoleRule(profile, "realtime_prediction.current_field");
  if (!rule.empty()) {
    return fieldDisplayRoleMatches(item, rule);
  }
  const std::string field = jsonString(item, "field_name");
  const std::string port = jsonString(item, "port_id");
  const std::string contract = jsonString(item, "contract_id");
  return !field.empty() || port.find("field") != std::string::npos ||
         contract.find("field") != std::string::npos ||
         contract.find("Field") != std::string::npos;
}

bool isFuturePredictionBranchId(const std::string& branch_id) {
  return startsWith(branch_id, "predict.");
}

double jsonArrayDouble(const json& value, std::size_t index, double fallback = std::numeric_limits<double>::quiet_NaN()) {
  if (!value.is_array() || index >= value.size() || !value.at(index).is_number()) {
    return fallback;
  }
  return value.at(index).get<double>();
}

std::string compactNumber(double value, int precision = 3) {
  if (!std::isfinite(value)) {
    return "";
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  std::string text = out.str();
  while (text.size() > 1 && text.back() == '0') {
    text.pop_back();
  }
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
  }
  return text;
}

double stateLabelMappedNumber(const json& state_output, const json& mapping) {
  const json values = state_output.value("values", json::object());
  const std::string value_key = jsonString(mapping, "value_key");
  if (!value_key.empty()) {
    const double direct = jsonDouble(values, value_key, std::numeric_limits<double>::quiet_NaN());
    if (std::isfinite(direct)) {
      return direct;
    }
  }
  const std::string object_key = jsonString(mapping, "object_key");
  if (!object_key.empty()) {
    const json obj = state_output.value(object_key, json::object());
    const std::string object_value_key = jsonString(mapping, "object_value_key");
    if (!object_value_key.empty()) {
      const double nested = jsonDouble(obj, object_value_key, std::numeric_limits<double>::quiet_NaN());
      if (std::isfinite(nested)) {
        return nested;
      }
    }
    const std::string object_array_key = jsonString(mapping, "object_array_key");
    if (!object_array_key.empty()) {
      const int index = jsonInt(mapping, "object_array_index", -1);
      if (index >= 0) {
        const double nested =
            jsonArrayDouble(obj.value(object_array_key, json::array()),
                            static_cast<std::size_t>(index));
        if (std::isfinite(nested)) {
          return nested;
        }
      }
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

json stateLabelFromStateOutput(const json& state_output, const json& profile) {
  if (!state_output.is_object()) {
    return json::object();
  }
  const json values = state_output.value("values", json::object());
  json label = json::object();

  auto add_number = [&label](const std::string& key, double value) {
    if (std::isfinite(value)) {
      label[key] = value;
    }
  };

  const json state_label = objectOrEmpty(profile).value("state_label", json::object());
  const json mappings = state_label.value("value_mappings", json::array());
  if (mappings.is_array() && !mappings.empty()) {
    for (const auto& mapping : mappings) {
      if (!mapping.is_object()) {
        continue;
      }
      const std::string out_key = jsonString(mapping, "out");
      if (!out_key.empty()) {
        add_number(out_key, stateLabelMappedNumber(state_output, mapping));
      }
    }
    for (const std::string object_key : jsonStringArray(state_label, "state_object_keys")) {
      if (state_output.contains(object_key) && state_output.at(object_key).is_object()) {
        label[object_key] = state_output.at(object_key);
      }
    }
  } else {
    const double state_time_s =
        jsonDouble(values, "time_s", jsonDouble(values, "time", std::numeric_limits<double>::quiet_NaN()));
    add_number("state_time_s", state_time_s);
    add_number("time_s", state_time_s);
  }
  if (label.empty()) {
    return label;
  }

  std::vector<std::string> parts;
  const json label_parts = state_label.value("label_parts", json::array());
  if (label_parts.is_array() && !label_parts.empty()) {
    for (const auto& part : label_parts) {
      if (!part.is_object()) {
        continue;
      }
      const std::string key = jsonString(part, "key");
      const double value = key.empty() ? std::numeric_limits<double>::quiet_NaN()
                                      : jsonDouble(label, key, std::numeric_limits<double>::quiet_NaN());
      if (std::isfinite(value)) {
        parts.push_back(jsonString(part, "prefix") +
                        compactNumber(value, jsonInt(part, "precision", 3)) +
                        jsonString(part, "suffix"));
      }
    }
  } else {
    const double state_time_s = jsonDouble(label, "state_time_s", std::numeric_limits<double>::quiet_NaN());
    if (std::isfinite(state_time_s)) {
      parts.push_back("t=" + compactNumber(state_time_s, 3) + "s");
    }
  }
  std::ostringstream text;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      text << ", ";
    }
    text << parts[i];
  }
  label["label_text"] = text.str();
  return label;
}

json stateLabelFromOnlineFrame(const json& frame, const json& profile) {
  const json payload =
      frame.is_object() && frame.contains("frame") && frame.at("frame").is_object()
          ? frame.at("frame")
          : frame;
  json selected = payload.value("selected_state", json::object());
  if (!selected.is_object()) {
    selected = json::object();
  }
  if (!selected.contains("time") && payload.contains("sample_time_s")) {
    selected["time"] = payload.at("sample_time_s");
  }
  return stateLabelFromStateOutput({{"values", selected}}, profile);
}

std::map<int, json> futureStateLabelsByStep(const fs::path& run_dir, const json& profile) {
  std::map<int, json> labels;
  const json source = objectOrEmpty(profile).value("future_state_label_source", json::object());
  std::vector<std::string> producer_nodes = jsonStringArray(source, "producer_nodes");
  if (producer_nodes.empty()) {
    return labels;
  }
  const std::string output_port = jsonString(source, "output_port", "state.next");
  auto collect_packets = [&](const fs::path& packets_path, bool append_in_order, int& append_index) {
    const json packets = objectOrEmpty(readJsonIfExists(packets_path));
    const json packet_array = arrayOrEmpty(packets.value("packets", json::array()));
    if (!packet_array.is_array()) {
      return;
    }
    int fallback_index = 0;
    for (const auto& packet : packet_array) {
      if (!packet.is_object()) {
        continue;
      }
      const std::string node =
          jsonString(packet, "producer_node", jsonString(packet, "node_id", ""));
      if (!stringListContains(producer_nodes, node)) {
        continue;
      }
      const json payload = objectOrEmpty(packet.value("payload", json::object()));
      const json output_ports = objectOrEmpty(payload.value("output_ports", json::object()));
      const json state_next = objectOrEmpty(output_ports.value(output_port, json::object()));
      const json label = stateLabelFromStateOutput(state_next, profile);
      if (label.empty()) {
        continue;
      }
      int step_index = append_in_order ? append_index : jsonInt(packet, "tick_index", 0) - 1;
      if (step_index < 0) {
        step_index = fallback_index;
      }
      json enriched = label;
      enriched["future_frame_index"] = step_index;
      const double relative_time_s = jsonDouble(packet, "time_s", std::numeric_limits<double>::quiet_NaN());
      const double source_time_s = jsonDouble(packet, "source_time_s", std::numeric_limits<double>::quiet_NaN());
      if (std::isfinite(relative_time_s)) {
        enriched["relative_time_s"] = relative_time_s;
      }
      if (std::isfinite(source_time_s)) {
        enriched["source_time_s"] = source_time_s;
      }
      enriched["tick_index"] = jsonInt(packet, "tick_index", step_index + 1);
      labels[step_index] = enriched;
      ++fallback_index;
      if (append_in_order) {
        ++append_index;
      }
    }
  };

  int root_append_index = 0;
  collect_packets(run_dir / "runtime_packets.json", false, root_append_index);
  if (!labels.empty()) {
    return labels;
  }

  const fs::path chunks_root = run_dir / "_branch_chunks";
  if (!fs::exists(chunks_root) || !fs::is_directory(chunks_root)) {
    return labels;
  }
  std::vector<fs::path> chunk_dirs;
  for (const auto& entry : fs::directory_iterator(chunks_root)) {
    if (entry.is_directory()) {
      chunk_dirs.push_back(entry.path());
    }
  }
  std::sort(chunk_dirs.begin(), chunk_dirs.end());
  int append_index = 0;
  for (const auto& chunk_dir : chunk_dirs) {
    collect_packets(chunk_dir / "runtime_packets.json", true, append_index);
  }
  return labels;
}

void applyStateLabel(json& item, const json& label) {
  if (!item.is_object() || !label.is_object() || label.empty()) {
    return;
  }
  item["state_label"] = label;
  for (auto it = label.begin(); it != label.end(); ++it) {
    if (it.key() != "label_text" &&
        (it.value().is_number() || it.value().is_string() || it.value().is_boolean())) {
      item[it.key()] = it.value();
    }
  }
  if (label.contains("label_text")) {
    item["label_text"] = label.at("label_text");
  }
  if (!item.contains("time_point") && label.contains("relative_time_s")) {
    item["time_point"] = {
        {"run_time_s", label.value("relative_time_s", 0.0)},
        {"source_time_s", label.value("source_time_s", label.value("relative_time_s", 0.0))},
        {"tick_index", label.value("tick_index", 0)},
    };
  }
}

std::string displayRoleForPredictionArtifact(const json& item, const json& profile) {
  const json rules = objectOrEmpty(profile).value("field_display_roles", json::array());
  if (rules.is_array()) {
    for (const auto& rule : rules) {
      if (!rule.is_object()) {
        continue;
      }
      const std::string role = jsonString(rule, "display_role");
      if (role == "realtime_prediction.current_field") {
        continue;
      }
      if (!role.empty() && fieldDisplayRoleMatches(item, rule)) {
        return role;
      }
    }
  }
  const std::string field = jsonString(item, "field_name", "");
  const std::string contract = jsonString(item, "contract_id", "");
  if (!field.empty() || contract.find("field") != std::string::npos ||
      contract.find("Field") != std::string::npos) {
    return "future_prediction.field";
  }
  return "";
}

int countItemsForBranch(const json& items, const std::string& branch_id) {
  if (!items.is_array()) {
    return 0;
  }
  int count = 0;
  for (const auto& item : items) {
    if (item.is_object() && jsonString(item, "branch_id") == branch_id) {
      ++count;
    }
  }
  return count;
}

std::string inferRuntimeBranchKind(const std::string& workflow_id, const json& sensor_stream, const json& profile) {
  const json role = workflowRoleForWorkflowId(profile, workflow_id);
  if (!role.empty()) {
    const std::string kind = jsonString(role, "branch_kind");
    if (!kind.empty()) {
      return kind;
    }
  }
  const bool has_frames =
      sensor_stream.is_object() && sensor_stream.contains("frames") &&
      sensor_stream.at("frames").is_array() && !sensor_stream.at("frames").empty();
  if (has_frames) {
    const json roles = objectOrEmpty(profile).value("workflow_roles", json::array());
    if (roles.is_array()) {
      for (const auto& item : roles) {
        if (item.is_object() && jsonBool(item, "requires_sensor_stream", false)) {
          const std::string kind = jsonString(item, "branch_kind");
          if (!kind.empty()) {
            return kind;
          }
        }
      }
    }
  }
  if (has_frames) {
    return "online_mainline";
  }
  return "single_run";
}

json branchTemplate(const json& profile, const std::string& key) {
  const json templates = objectOrEmpty(profile).value("branch_templates", json::object());
  if (templates.is_object() && templates.contains(key) && templates.at(key).is_object()) {
    return templates.at(key);
  }
  return json::object();
}

json applyBranchTemplate(json record, const json& profile, const std::string& key) {
  const json tmpl = branchTemplate(profile, key);
  if (!tmpl.is_object()) {
    return record;
  }
  for (const std::string field : {"display_name", "kind_label", "parent_branch_id"}) {
    const std::string value = jsonString(tmpl, field);
    if (!value.empty()) {
      record[field] = value;
    }
  }
  if (tmpl.contains("branch_roles") && tmpl.at("branch_roles").is_array()) {
    record["branch_roles"] = tmpl.at("branch_roles");
  }
  if (tmpl.contains("priority") && tmpl.at("priority").is_number()) {
    record["priority"] = tmpl.at("priority");
  }
  return record;
}

std::string branchParentFromProfile(const json& profile,
                                    const std::string& key,
                                    const std::string& fallback) {
  const json tmpl = branchTemplate(profile, key);
  const std::string value = jsonString(tmpl, "parent_branch_id");
  return value.empty() ? fallback : value;
}

bool matchesAccumulatedStatePolicy(const json& item, const json& profile) {
  const json policy = objectOrEmpty(objectOrEmpty(profile).value("health_ledger", json::object()))
                          .value("accumulated_state_match", json::object());
  if (!policy.is_object() || policy.empty()) {
    return false;
  }
  const std::string node = jsonString(item, "node_id");
  const std::string op = jsonString(item, "operator_id");
  if (containsAnyToken(node, jsonStringArray(policy, "node_contains")) ||
      containsAnyToken(op, jsonStringArray(policy, "operator_contains"))) {
    return true;
  }
  return false;
}

int extractFrameIndexFromSeedPath(const std::string& seed_runtime_outputs_ref) {
  const std::string token = "online_frame_";
  const std::size_t pos = seed_runtime_outputs_ref.find(token);
  if (pos == std::string::npos) {
    return -1;
  }
  std::size_t start = pos + token.size();
  std::size_t end = start;
  while (end < seed_runtime_outputs_ref.size() &&
         std::isdigit(static_cast<unsigned char>(seed_runtime_outputs_ref[end]))) {
    ++end;
  }
  if (end == start) {
    return -1;
  }
  try {
    return std::stoi(seed_runtime_outputs_ref.substr(start, end - start));
  } catch (...) {
    return -1;
  }
}

void addSeriesPoint(json& series_by_id,
                    const std::string& series_id,
                    const std::string& label,
                    double value,
                    double time_s,
                    const std::string& source_ref,
                    const std::string& branch_id,
                    const std::string& point_kind,
                    int step_index,
                    const std::string& unit = "") {
  if (!std::isfinite(value)) {
    return;
  }
  if (!series_by_id.contains(series_id)) {
    series_by_id[series_id] = {
        {"series_id", series_id},
        {"label", label},
        {"branch_id", branch_id},
        {"unit", unit},
        {"source_ref", source_ref},
        {"points", json::array()},
    };
  }
  json point = {
      {"time_s", time_s},
      {"value", value},
      {"source_ref", source_ref},
      {"branch_id", branch_id},
      {"point_kind", point_kind},
  };
  if (step_index >= 0) {
    point["step_index"] = step_index;
  }
  series_by_id[series_id]["points"].push_back(point);
}

void offsetTimePoint(json& point, double time_offset_s, int tick_offset) {
  if (!point.is_object()) {
    return;
  }
  if (point.contains("run_time_s") && point.at("run_time_s").is_number()) {
    point["run_time_s"] = point.at("run_time_s").get<double>() + time_offset_s;
  }
  if (point.contains("source_time_s") && point.at("source_time_s").is_number()) {
    point["source_time_s"] = point.at("source_time_s").get<double>() + time_offset_s;
  }
  if (point.contains("tick_index") && point.at("tick_index").is_number_integer()) {
    point["tick_index"] = point.at("tick_index").get<int>() + tick_offset;
  }
}

double publicTimeFromItem(const json& item, double fallback = 0.0) {
  if (!item.is_object()) {
    return fallback;
  }
  const double public_time = jsonDouble(item, "public_time_s", std::numeric_limits<double>::quiet_NaN());
  if (std::isfinite(public_time)) {
    return public_time;
  }
  const double public_output_time =
      jsonDouble(item, "public_output_time_s", std::numeric_limits<double>::quiet_NaN());
  if (std::isfinite(public_output_time)) {
    return public_output_time;
  }
  const json time_point = item.value("time_point", json::object());
  const double point_time = jsonDouble(time_point, "run_time_s", std::numeric_limits<double>::quiet_NaN());
  if (std::isfinite(point_time)) {
    return point_time;
  }
  return jsonDouble(item, "run_time_s", fallback);
}

void offsetNumericTimeField(json& item, const std::string& key, double time_offset_s) {
  if (item.is_object() && item.contains(key) && item.at(key).is_number()) {
    item[key] = item.at(key).get<double>() + time_offset_s;
  }
}

void offsetPublicTimingMetadata(json& item, double time_offset_s) {
  for (const std::string key : {"public_time_s", "public_output_time_s", "run_time_s",
                                "source_time_s", "state_time_s"}) {
    offsetNumericTimeField(item, key, time_offset_s);
  }
  if (item.contains("time_summary") && item.at("time_summary").is_object()) {
    for (const std::string key : {"public_time_s", "public_output_time_s", "run_time_s"}) {
      offsetNumericTimeField(item["time_summary"], key, time_offset_s);
    }
    if (item["time_summary"].contains("public_tick") &&
        item["time_summary"]["public_tick"].is_object()) {
      offsetNumericTimeField(item["time_summary"]["public_tick"], "output_time_s", time_offset_s);
    }
  }
  if (item.contains("public_tick") && item.at("public_tick").is_object()) {
    offsetNumericTimeField(item["public_tick"], "output_time_s", time_offset_s);
  }
  if (item.contains("time_info") && item.at("time_info").is_object()) {
    offsetPublicTimingMetadata(item["time_info"], time_offset_s);
  }
}

void offsetNestedTime(json& item, double time_offset_s, int tick_offset) {
  if (!item.is_object()) {
    return;
  }
  offsetPublicTimingMetadata(item, time_offset_s);
  if (item.contains("time_point")) {
    offsetTimePoint(item["time_point"], time_offset_s, tick_offset);
  }
  if (item.contains("output_time_point")) {
    offsetTimePoint(item["output_time_point"], time_offset_s, tick_offset);
  }
  if (item.contains("time_info") && item.at("time_info").is_object()) {
    if (item["time_info"].contains("time_point")) {
      offsetTimePoint(item["time_info"]["time_point"], time_offset_s, tick_offset);
    }
    if (item["time_info"].contains("output_time_point")) {
      offsetTimePoint(item["time_info"]["output_time_point"], time_offset_s, tick_offset);
    }
  }
}

void setTimePointToPublicTime(json& point, double public_time_s, int tick_index) {
  if (!point.is_object()) {
    point = json::object();
  }
  point["run_time_s"] = public_time_s;
  point["source_time_s"] = public_time_s;
  point["tick_index"] = tick_index;
  point["stamp_ns"] = static_cast<long long>(public_time_s * 1.0e9);
}

void forceMainlinePublicTime(json& item, double public_time_s, int tick_index) {
  if (!item.is_object() || !std::isfinite(public_time_s)) {
    return;
  }
  const double local_public_time = publicTimeFromItem(item, public_time_s);
  item["branch_relative_time_s"] = local_public_time;
  item["public_time_s"] = public_time_s;
  item["public_output_time_s"] = public_time_s;
  item["run_time_s"] = public_time_s;
  item["source_time_s"] = public_time_s;
  if (item.contains("sample_time_s") && item.at("sample_time_s").is_number()) {
    item["sample_time_s"] = public_time_s;
  }
  setTimePointToPublicTime(item["time_point"], public_time_s, tick_index);
  if (item.contains("output_time_point")) {
    setTimePointToPublicTime(item["output_time_point"], public_time_s, tick_index);
  }
  if (!item.contains("time_summary") || !item["time_summary"].is_object()) {
    item["time_summary"] = json::object();
  }
  item["time_summary"]["public_time_s"] = public_time_s;
  item["time_summary"]["public_output_time_s"] = public_time_s;
  item["time_summary"]["branch_relative_time_s"] = local_public_time;
  if (item.contains("time_info") && item.at("time_info").is_object()) {
    forceMainlinePublicTime(item["time_info"], public_time_s, tick_index);
  }
}

void appendArrayWithMetadata(json& target,
                             const json& source,
                             int global_iteration_offset,
                             int chunk_index,
                             const fs::path& chunk_dir,
                             double time_offset_s) {
  if (!source.is_array()) {
    return;
  }
  for (auto item : source) {
    if (!item.is_object()) {
      continue;
    }
    if (item.contains("loop_iteration_index") && item.at("loop_iteration_index").is_number_integer()) {
      const int global_iteration = item.at("loop_iteration_index").get<int>() + global_iteration_offset;
      item["loop_iteration_index"] = global_iteration;
      item["iteration_index"] = global_iteration;
    } else if (item.contains("iteration_index") && item.at("iteration_index").is_number_integer()) {
      const int global_iteration = item.at("iteration_index").get<int>() + global_iteration_offset;
      item["iteration_index"] = global_iteration;
      item["loop_iteration_index"] = global_iteration;
    }
    offsetNestedTime(item, time_offset_s, global_iteration_offset);
    item["branch_chunk_index"] = chunk_index;
    item["branch_chunk_run_dir"] = pathString(chunk_dir);
    target.push_back(item);
  }
}

#ifdef _WIN32
std::wstring widenUtf8Local(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (required <= 0) {
    return fs::path(text).wstring();
  }
  std::wstring wide(static_cast<std::size_t>(required - 1), L'\0');
  (void)MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), required);
  return wide;
}
#endif

bool processStillRunning(unsigned long process_id) {
  if (process_id == 0) {
    return false;
  }
#ifdef _WIN32
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == nullptr) {
    return false;
  }
  DWORD exit_code = 0;
  const bool running = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
  CloseHandle(process);
  return running;
#else
  return false;
#endif
}

bool launchDetachedProcess(const std::vector<std::string>& args,
                           const fs::path& working_dir,
                           const fs::path& log_path,
                           unsigned long* process_id) {
  if (args.empty()) {
    return false;
  }
  std::ostringstream command;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i > 0) {
      command << ' ';
    }
    command << quoteArg(args[i]);
  }
#ifdef _WIN32
  std::wstring command_w = widenUtf8Local(command.str());
  std::vector<wchar_t> command_buffer(command_w.begin(), command_w.end());
  command_buffer.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  SECURITY_ATTRIBUTES stdio_security{};
  stdio_security.nLength = sizeof(stdio_security);
  stdio_security.bInheritHandle = TRUE;
  HANDLE nul_stdio = CreateFileW(
      L"NUL",
      GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      &stdio_security,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  HANDLE log_stdio = INVALID_HANDLE_VALUE;
  if (!log_path.empty()) {
    std::error_code ec;
    fs::create_directories(log_path.parent_path(), ec);
    log_stdio = CreateFileW(
        log_path.wstring().c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &stdio_security,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
  }
  if (nul_stdio != INVALID_HANDLE_VALUE) {
    startup.dwFlags |= STARTF_USESTDHANDLES;
    startup.hStdInput = nul_stdio;
    startup.hStdOutput = log_stdio != INVALID_HANDLE_VALUE ? log_stdio : nul_stdio;
    startup.hStdError = log_stdio != INVALID_HANDLE_VALUE ? log_stdio : nul_stdio;
  }
  PROCESS_INFORMATION process{};
  const fs::path cwd = working_dir.empty() ? fs::current_path() : fs::absolute(working_dir);
  std::wstring cwd_w = cwd.wstring();
  DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
  BOOL created = CreateProcessW(
      nullptr,
      command_buffer.data(),
      nullptr,
      nullptr,
      nul_stdio != INVALID_HANDLE_VALUE ? TRUE : FALSE,
      flags,
      nullptr,
      cwd_w.c_str(),
      &startup,
      &process);
  if (!created && GetLastError() == ERROR_ACCESS_DENIED) {
    flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    created = CreateProcessW(
        nullptr,
        command_buffer.data(),
        nullptr,
        nullptr,
        nul_stdio != INVALID_HANDLE_VALUE ? TRUE : FALSE,
        flags,
        nullptr,
        cwd_w.c_str(),
        &startup,
        &process);
  }
  if (!created) {
    if (nul_stdio != INVALID_HANDLE_VALUE) {
      CloseHandle(nul_stdio);
    }
    if (log_stdio != INVALID_HANDLE_VALUE) {
      CloseHandle(log_stdio);
    }
    return false;
  }
  if (process_id != nullptr) {
    *process_id = static_cast<unsigned long>(process.dwProcessId);
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (nul_stdio != INVALID_HANDLE_VALUE) {
    CloseHandle(nul_stdio);
  }
  if (log_stdio != INVALID_HANDLE_VALUE) {
    CloseHandle(log_stdio);
  }
  return true;
#else
  std::string shell_command = command.str() + " >/dev/null 2>&1 &";
  const int rc = std::system(shell_command.c_str());
  if (process_id != nullptr) {
    *process_id = 0;
  }
  return rc == 0;
#endif
}

class ScopedDirectoryLock {
 public:
  explicit ScopedDirectoryLock(fs::path lock_dir) : lock_dir_(std::move(lock_dir)) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
      std::error_code ec;
      if (fs::create_directory(lock_dir_, ec)) {
        locked_ = true;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("Timed out waiting for runtime index merge lock: " + pathString(lock_dir_));
  }

  ~ScopedDirectoryLock() {
    if (locked_) {
      std::error_code ec;
      fs::remove(lock_dir_, ec);
    }
  }

  ScopedDirectoryLock(const ScopedDirectoryLock&) = delete;
  ScopedDirectoryLock& operator=(const ScopedDirectoryLock&) = delete;

 private:
  fs::path lock_dir_;
  bool locked_ = false;
};

int countFieldArtifacts(const fs::path& run_dir) {
  const fs::path root = run_dir / "field_artifacts";
  if (!fs::exists(root)) {
    return 0;
  }
  int count = 0;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      ++count;
    }
  }
  return count;
}

int countFieldArtifactEntries(const json& data_plane_manifest) {
  int count = 0;
  const json entries = data_plane_manifest.value("entries", json::array());
  if (!entries.is_array()) {
    return 0;
  }
  for (const auto& entry : entries) {
    if (!entry.is_object()) {
      continue;
    }
    const std::string representation = jsonString(entry, "representation", "");
    const std::string contract = jsonString(entry, "contract_id", "");
    if (representation == "artifact_ref" ||
        contract.find("field") != std::string::npos ||
        contract.find("Field") != std::string::npos) {
      ++count;
    }
  }
  return count;
}

int countHeldOutputsInLoop(const json& runtime_loop_summary) {
  int count = 0;
  const json iterations = runtime_loop_summary.value("iterations", json::array());
  if (!iterations.is_array()) {
    return 0;
  }
  for (const auto& iteration : iterations) {
    if (iteration.is_object()) {
      count += jsonInt(iteration, "held_output_count", 0);
    }
  }
  return count;
}

double workflowBaseDtS(const json& workflow_snapshot) {
  const json workflow = workflowFromSnapshot(workflow_snapshot);
  const json solver = workflow.value("solver_policy", json::object());
  return jsonDouble(solver, "base_dt_s", 0.0);
}

double workflowOutputPeriodS(const json& workflow_snapshot) {
  const json workflow = workflowFromSnapshot(workflow_snapshot);
  const json solver = workflow.value("solver_policy", json::object());
  return jsonDouble(solver, "output_period_s", jsonDouble(solver, "base_dt_s", 0.0));
}

std::vector<std::string> splitArgs(int argc, char** argv) {
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    out.emplace_back(argv[i]);
  }
  return out;
}

std::string requireOption(const std::map<std::string, std::string>& args, const std::string& name) {
  const auto it = args.find(name);
  if (it == args.end() || it->second.empty()) {
    throw std::runtime_error("Missing required option: --" + name);
  }
  return it->second;
}

std::map<std::string, std::string> parseOptions(int argc, char** argv) {
  std::map<std::string, std::string> out;
  const auto parts = splitArgs(argc, argv);
  for (std::size_t i = 1; i < parts.size(); ++i) {
    std::string key = parts[i];
    if (key.rfind("--", 0) != 0) {
      continue;
    }
    key = key.substr(2);
    if (key == "prepare-only" || key == "preflight-adapters" ||
        key == "require-adapter-registry" || key == "no-require-adapter-registry" ||
        key == "replay-by-platform-clock" || key == "allow-legacy-process-backend" ||
        key == "branch-worker" || key == "resume-existing-branch" ||
        key == "wait-for-branches" || key == "no-branch-manager") {
      out[key] = "true";
      continue;
    }
    if (i + 1 >= parts.size()) {
      out[key] = "";
      continue;
    }
    out[key] = parts[++i];
  }
  return out;
}

void printUsage() {
  std::cout
      << "FlightEnvPlatformRuntimeHost\n"
      << "  --workspace-root <path>\n"
      << "  --run-id-prefix <id>\n"
      << "  --object-package-root <path>  # or FLIGHTENV_OBJECT_PACKAGE_ROOT\n"
      << "  --compiled-online <dir>\n"
      << "  --compiled-future <dir>\n"
      << "  --external-observation-stream <sensor_stream.json>\n"
      << "  --online-frames <N>\n"
      << "  --prediction-every-frames <N>\n"
      << "  --future-max-iterations <N>\n"
      << "  [--execution-backend native_adapter_sessions]\n"
      << "  [--zero-copy-mode auto|prefer|require|off]\n"
      << "  [--typed-buffer-persistence shadow_artifact|memory_only]\n"
      << "  [--execution-backend compiled_workflow_process_backend --allow-legacy-process-backend]  # compatibility only\n"
      << "  [--preflight-adapters]\n"
      << "  [--branch-chunk-iterations <N>]\n"
      << "  [--wait-for-branches]\n"
      << "  [--branch-control stop|resume|status --branch-id <id>]\n";
}

}  // namespace

PlatformRuntimeHost::PlatformRuntimeHost(HostOptions options) : options_(std::move(options)) {}

PlatformRuntimeHost::StateLock::StateLock(PlatformRuntimeHost& host) : host_(host) {
  while (host_.state_lock_.test_and_set(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

PlatformRuntimeHost::StateLock::~StateLock() {
  host_.state_lock_.clear(std::memory_order_release);
}

void PlatformRuntimeHost::resolveDefaults() {
  if (options_.workspace_root.empty()) {
    options_.workspace_root = fs::current_path();
  }
  options_.workspace_root = fs::absolute(options_.workspace_root);
  if (options_.pdk_root.empty()) {
    options_.pdk_root = options_.workspace_root / "flightenv-platform-pdk";
  }
  options_.pdk_root = fs::absolute(options_.pdk_root);
  if (options_.object_package_root.empty()) {
    const char* env_object_root = std::getenv("FLIGHTENV_OBJECT_PACKAGE_ROOT");
    if (env_object_root && *env_object_root) {
      options_.object_package_root = fs::path(env_object_root);
    } else {
      env_object_root = std::getenv("FLIGHTENV_PLATFORM_OBJECT_ROOT");
      if (env_object_root && *env_object_root) {
        options_.object_package_root = fs::path(env_object_root);
      }
    }
    if (options_.object_package_root.empty()) {
      throw std::runtime_error(
          "object package root is required; pass --object-package-root or set "
          "FLIGHTENV_OBJECT_PACKAGE_ROOT/FLIGHTENV_PLATFORM_OBJECT_ROOT");
    }
  }
  options_.object_package_root = fs::absolute(options_.object_package_root);
  object_runtime_profile_ = loadObjectRuntimeProfile(options_.object_package_root);
  if (options_.compiled_online_workflow.empty()) {
    const fs::path profile_path =
        compiledWorkflowPathForRole(object_runtime_profile_, options_.workspace_root, "online_mainline");
    options_.compiled_online_workflow =
        profile_path.empty()
            ? options_.workspace_root / "_local_artifacts/platform-pdk/compiled-workflows/online_mainline"
            : profile_path;
  }
  if (options_.compiled_future_workflow.empty()) {
    const fs::path profile_path =
        compiledWorkflowPathForRole(object_runtime_profile_, options_.workspace_root, "future_prediction");
    options_.compiled_future_workflow =
        profile_path.empty()
            ? options_.workspace_root / "_local_artifacts/platform-pdk/compiled-workflows/future_prediction"
            : profile_path;
  }
  if (options_.adapter_registry.empty()) {
    const fs::path profile_path =
        firstExistingProfilePath(object_runtime_profile_,
                                 options_.workspace_root,
                                 options_.object_package_root,
                                 "adapter_registry_candidates");
    options_.adapter_registry =
        profile_path.empty()
            ? options_.object_package_root / "tools/adapter_registries/adapter_registry.json"
            : profile_path;
  }
  if (options_.pdk_run_script.empty()) {
    options_.pdk_run_script = options_.pdk_root / "tools/run_compiled_workflow.ps1";
  }
  if (options_.pdk_cli.empty()) {
    options_.pdk_cli = options_.pdk_root / "tools/flightenv-platform-cli/flightenv-platform-cli.py";
  }
  if (options_.run_root.empty()) {
    options_.run_root = options_.workspace_root / "_local_artifacts/platform-runtime/runtime-host-runs";
  }
  if (options_.chain_dir.empty()) {
    options_.chain_dir =
        options_.workspace_root / "_local_artifacts/platform-runtime/mainline-runs" / options_.run_id_prefix;
  }
  if (options_.runtime_host_exe.empty()) {
    options_.runtime_host_exe =
        options_.workspace_root / "_deps/workspace/x64/Release/FlightEnvPlatformRuntimeHost.exe";
  }
  options_.compiled_online_workflow = fs::absolute(options_.compiled_online_workflow);
  options_.compiled_future_workflow = fs::absolute(options_.compiled_future_workflow);
  options_.adapter_registry = fs::absolute(options_.adapter_registry);
  options_.pdk_run_script = fs::absolute(options_.pdk_run_script);
  options_.pdk_cli = fs::absolute(options_.pdk_cli);
  options_.run_root = fs::absolute(options_.run_root);
  options_.chain_dir = fs::absolute(options_.chain_dir);
  options_.runtime_host_exe = fs::absolute(options_.runtime_host_exe);
  if (!options_.branch_run_dir.empty()) {
    options_.branch_run_dir = fs::absolute(options_.branch_run_dir);
  }
  if (!options_.seed_runtime_outputs.empty()) {
    options_.seed_runtime_outputs = fs::absolute(options_.seed_runtime_outputs);
  }
  if (!options_.external_observation_stream.empty()) {
    options_.external_observation_stream = fs::absolute(options_.external_observation_stream);
  } else if (!options_.prepare_only && !options_.branch_worker && options_.branch_control_action.empty()) {
    const fs::path profile_path =
        firstExistingProfilePath(object_runtime_profile_,
                                 options_.workspace_root,
                                 options_.object_package_root,
                                 "external_observation_stream_candidates");
    if (!profile_path.empty()) {
      options_.external_observation_stream = fs::absolute(profile_path);
    }
  }
  options_.branch_chunk_iterations = std::max(1, options_.branch_chunk_iterations);
  (void)resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode);
  (void)resolveRuntimeTypedBufferPersistence(options_.typed_buffer_persistence);
  if (options_.execution_backend != "native_adapter_sessions" &&
      options_.execution_backend != "compiled_workflow_process_backend") {
    throw std::runtime_error(
        "Unsupported execution backend: " + options_.execution_backend +
        " (expected native_adapter_sessions or compiled_workflow_process_backend)");
  }
  if (options_.execution_backend == "compiled_workflow_process_backend" &&
      !options_.allow_legacy_process_backend) {
    throw std::runtime_error(
        "compiled_workflow_process_backend is deprecated and disabled by default. "
        "Use native_adapter_sessions for production runs, or pass "
        "--allow-legacy-process-backend only for historical replay/migration diagnostics.");
  }
}

void PlatformRuntimeHost::ensureInputs() const {
  const std::vector<std::pair<fs::path, std::string>> required = {
      {options_.pdk_root, "PDK root"},
      {options_.compiled_online_workflow, "compiled online workflow"},
      {options_.compiled_future_workflow, "compiled future workflow"},
      {options_.adapter_registry, "adapter registry"},
      {options_.pdk_cli, "PDK CLI"},
  };
  for (const auto& item : required) {
    if (!fs::exists(item.first)) {
      throw std::runtime_error(item.second + " not found: " + pathString(item.first));
    }
  }
  if (!options_.prepare_only && !options_.branch_worker && options_.branch_control_action.empty() &&
      options_.external_observation_stream.empty()) {
    throw std::runtime_error("External observation stream is required for online execution.");
  }
  if (!options_.external_observation_stream.empty() && !fs::exists(options_.external_observation_stream)) {
    throw std::runtime_error(
        "External observation stream not found: " + pathString(options_.external_observation_stream));
  }
  if (!useNativeBackend() && !fs::exists(options_.pdk_run_script)) {
    throw std::runtime_error("PDK run script not found: " + pathString(options_.pdk_run_script));
  }
}

void PlatformRuntimeHost::loadCompiledWorkflowMetadata() {
  online_workflow_snapshot_ = readJson(options_.compiled_online_workflow / "workflow_snapshot.json");
  future_workflow_snapshot_ = readJson(options_.compiled_future_workflow / "workflow_snapshot.json");
  const json online_workflow = workflowFromSnapshot(online_workflow_snapshot_);
  const json future_workflow = workflowFromSnapshot(future_workflow_snapshot_);
  online_workflow_id_ = jsonString(online_workflow_snapshot_, "workflow_id",
                                   jsonString(online_workflow, "workflow_id", "online"));
  future_workflow_id_ = jsonString(future_workflow_snapshot_, "workflow_id",
                                   jsonString(future_workflow, "workflow_id", "future"));
  object_id_ = jsonString(online_workflow_snapshot_, "object_id",
                          jsonString(online_workflow, "object_id", "object"));

  if (options_.online_frames <= 0) {
    options_.online_frames = solverMaxSteps(online_workflow, 0);
  }
  if (options_.future_max_iterations <= 0) {
    options_.future_max_iterations = solverMaxSteps(future_workflow, 1);
  }

  const json branching = online_workflow.value("branching_policy", json::object());
  automatic_branching_enabled_ = jsonBool(branching, "enabled", false);
  branch_trigger_kind_ = jsonString(branching, "trigger_kind", automatic_branching_enabled_ ? "every_n_frames" : "disabled");
  if (branch_trigger_kind_ == "disabled" || branch_trigger_kind_ == "manual") {
    automatic_branching_enabled_ = false;
  }
  effective_prediction_every_frames_ = options_.prediction_every_frames > 0
                                           ? options_.prediction_every_frames
                                           : jsonInt(branching, "every_n_frames", 0);
  effective_prediction_time_interval_s_ = jsonDouble(branching, "time_interval_s", 0.0);
  effective_max_concurrent_branches_ = options_.max_concurrent_branches > 0
                                           ? options_.max_concurrent_branches
                                           : jsonInt(branching, "max_concurrent_branches", 1);
  if (automatic_branching_enabled_ && branch_trigger_kind_ == "every_n_frames") {
    effective_prediction_every_frames_ = std::max(1, effective_prediction_every_frames_);
  }
  effective_max_concurrent_branches_ = std::max(1, effective_max_concurrent_branches_);
}

int PlatformRuntimeHost::invokePdkRun(
    const fs::path& compiled_workflow,
    const fs::path& run_dir,
    const std::string& run_id,
    const fs::path& seed_runtime_outputs,
    const fs::path& external_observation_stream,
    int max_iterations,
    bool prepare_only) const {
  std::ostringstream command;
  command << "powershell.exe -NoProfile -ExecutionPolicy Bypass -File "
          << quoteArg(pathString(options_.pdk_run_script))
          << " -Python " << quoteArg(options_.python)
          << " -CompiledWorkflow " << quoteArg(pathString(compiled_workflow))
          << " -RunDir " << quoteArg(pathString(run_dir))
          << " -RunId " << quoteArg(run_id)
          << " -AdapterRegistry " << quoteArg(pathString(options_.adapter_registry));
  if (options_.require_adapter_registry) {
    command << " -RequireAdapterRegistry";
  }
  if (!seed_runtime_outputs.empty()) {
    command << " -SeedRuntimeOutputs " << quoteArg(pathString(seed_runtime_outputs));
  }
  if (!external_observation_stream.empty()) {
    command << " -ExternalObservationStream " << quoteArg(pathString(external_observation_stream));
  }
  if (max_iterations > 0) {
    command << " -RuntimeMaxIterations " << max_iterations;
  }
  if (prepare_only) {
    command << " -PrepareOnly";
  }
  std::cout << "[C++ RuntimeHost] 调用 workflow: " << run_id << std::endl;
  return std::system(command.str().c_str());
}

bool PlatformRuntimeHost::useNativeBackend() const {
  return options_.execution_backend == "native_adapter_sessions";
}

NativeWorkflowRunner& PlatformRuntimeHost::onlineNativeRunner() {
  if (!online_native_runner_) {
    NativeWorkflowOptions native_options;
    native_options.workspace_root = options_.workspace_root;
    native_options.pdk_root = options_.pdk_root;
    native_options.compiled_workflow = options_.compiled_online_workflow;
    native_options.adapter_registry = options_.adapter_registry;
    native_options.require_adapter_registry = options_.require_adapter_registry;
    native_options.python = options_.python;
    native_options.runtime_zero_copy_mode = options_.runtime_zero_copy_mode;
    native_options.typed_buffer_persistence = options_.typed_buffer_persistence;
    online_native_runner_ = std::make_unique<NativeWorkflowRunner>(std::move(native_options));
  }
  return *online_native_runner_;
}

void PlatformRuntimeHost::runCompiledWorkflow(
    const fs::path& compiled_workflow,
    NativeWorkflowRunner* runner,
    const fs::path& run_dir,
    const std::string& run_id,
    const fs::path& seed_runtime_outputs,
    const fs::path& external_observation_stream,
    int max_iterations,
    bool prepare_only,
    const std::string& branch_id,
    const std::string& timeline_id) {
  if (!useNativeBackend()) {
    const int rc = invokePdkRun(
        compiled_workflow,
        run_dir,
        run_id,
        seed_runtime_outputs,
        external_observation_stream,
        max_iterations,
        prepare_only);
    if (rc != 0) {
      throw std::runtime_error("Compiled workflow process backend failed: " + run_id);
    }
    return;
  }

  std::unique_ptr<NativeWorkflowRunner> local_runner;
  NativeWorkflowRunner* active_runner = runner;
  if (active_runner == nullptr) {
    NativeWorkflowOptions native_options;
    native_options.workspace_root = options_.workspace_root;
    native_options.pdk_root = options_.pdk_root;
    native_options.compiled_workflow = compiled_workflow;
    native_options.adapter_registry = options_.adapter_registry;
    native_options.require_adapter_registry = options_.require_adapter_registry;
    native_options.python = options_.python;
    native_options.runtime_zero_copy_mode = options_.runtime_zero_copy_mode;
    native_options.typed_buffer_persistence = options_.typed_buffer_persistence;
    local_runner = std::make_unique<NativeWorkflowRunner>(std::move(native_options));
    active_runner = local_runner.get();
  }

  NativeWorkflowRequest request;
  request.run_dir = run_dir;
  request.run_id = run_id;
  request.branch_id = branch_id;
  request.timeline_id = timeline_id;
  request.seed_runtime_outputs = seed_runtime_outputs;
  request.external_observation_stream = external_observation_stream;
  request.max_iterations = max_iterations;
  request.prepare_only = prepare_only;
  const NativeWorkflowResult result = active_runner->run(request);
  if (result.exit_code != 0) {
    throw std::runtime_error("Native workflow backend failed: " + run_id);
  }
}

void PlatformRuntimeHost::prepareRuntime() {
  fs::create_directories(options_.chain_dir);
  fs::create_directories(options_.run_root);
  std::cout << "[C++ RuntimeHost] 初始化 evidence 目录完成" << std::endl;
  current_stage_ = "initializing";
  current_message_ = "初始化算子并加载模型";
  std::cout << "[C++ RuntimeHost] 写入初始化进度..." << std::endl;
  writeProgressLocked();
  std::cout << "[C++ RuntimeHost] 初始化进度已写入" << std::endl;

  const fs::path prepare_root = options_.chain_dir / "prepare";
  const fs::path online_prepare = prepare_root / "online_filtering";
  const fs::path future_prepare = prepare_root / "future_prediction";
  const std::string online_prepare_id = options_.run_id_prefix + ".prepare.online";
  const std::string future_prepare_id = options_.run_id_prefix + ".prepare.future";
  json preflight_runs = json::array();

  if (options_.preflight_adapters) {
    std::cout << "[C++ RuntimeHost] 执行 adapter preflight..." << std::endl;
    NativeWorkflowRunner* online_runner = useNativeBackend() ? &onlineNativeRunner() : nullptr;
    runCompiledWorkflow(
        options_.compiled_online_workflow, online_runner, online_prepare, online_prepare_id, {}, {}, 0, true,
        kOnlineFilterBranchId, "preflight.online");
    runCompiledWorkflow(
        options_.compiled_future_workflow, nullptr, future_prepare, future_prepare_id, {}, {}, 0, true,
        "preflight.future", "preflight.future");
    preflight_runs = json::array({
        {
            {"workflow_role", "online_filtering"},
            {"run_id", online_prepare_id},
            {"run_dir", pathString(online_prepare)},
            {"runtime_evidence", pathString(online_prepare / "runtime_evidence.json")},
        },
        {
            {"workflow_role", "future_prediction"},
            {"run_id", future_prepare_id},
            {"run_dir", pathString(future_prepare)},
            {"runtime_evidence", pathString(future_prepare / "runtime_evidence.json")},
        },
    });
  }
  std::cout << "[C++ RuntimeHost] 写入 operator_initialization.json..." << std::endl;

  const json initialization = {
      {"schema_version", "flightenv.platform.cpp_runtime_initialization.v1"},
      {"run_id_prefix", options_.run_id_prefix},
      {"object_id", object_id_},
      {"generated_at_utc", nowUtcIso()},
      {"status", options_.preflight_adapters ? "preflight_completed" : "declared"},
      {"message",
       options_.preflight_adapters
           ? "C++ Runtime Host 已完成 workflow preflight；真实模型资源已按 adapter registry 加载校验。"
           : "C++ Runtime Host 已读取 compiled workflow、模型快照和资源锁；未执行重模型预热。需要真实预热时传入 --preflight-adapters。"},
      {"workflows",
       json::array({
           {
               {"workflow_role", "online_filtering"},
               {"workflow_id", online_workflow_id_},
               {"compiled_workflow_dir", pathString(options_.compiled_online_workflow)},
               {"prepare_run_dir", options_.preflight_adapters ? pathString(online_prepare) : ""},
               {"runtime_evidence", options_.preflight_adapters ? pathString(online_prepare / "runtime_evidence.json") : ""},
               {"operator_snapshot", pathString(options_.compiled_online_workflow / "operator_snapshot.json")},
               {"model_snapshot", pathString(options_.compiled_online_workflow / "model_snapshot.json")},
               {"resource_lock", pathString(options_.compiled_online_workflow / "resource_lock.json")},
               {"time_plan", pathString(options_.compiled_online_workflow / "time_plan.json")},
           },
           {
               {"workflow_role", "future_prediction"},
               {"workflow_id", future_workflow_id_},
               {"compiled_workflow_dir", pathString(options_.compiled_future_workflow)},
               {"prepare_run_dir", options_.preflight_adapters ? pathString(future_prepare) : ""},
               {"runtime_evidence", options_.preflight_adapters ? pathString(future_prepare / "runtime_evidence.json") : ""},
               {"operator_snapshot", pathString(options_.compiled_future_workflow / "operator_snapshot.json")},
               {"model_snapshot", pathString(options_.compiled_future_workflow / "model_snapshot.json")},
               {"resource_lock", pathString(options_.compiled_future_workflow / "resource_lock.json")},
               {"time_plan", pathString(options_.compiled_future_workflow / "time_plan.json")},
           },
      })},
      {"adapter_registry", pathString(options_.adapter_registry)},
      {"execution_backend", options_.execution_backend},
      {"native_session_note",
       useNativeBackend()
           ? "C++ Host directly manages adapter session lifecycle; DLL adapters stay in-process and PDK workflow process is not spawned."
           : "Compatibility mode: C++ Host keeps mainline/branch scheduling while operator execution uses the PDK compiled-workflow runner."},
      {"preflight_adapters", options_.preflight_adapters},
      {"preflight_runs", preflight_runs},
      {"external_input_contract",
       {
           {"driver_owner", "external"},
           {"accepted_input", "ObservationSnapshot.v1 / sensor_stream.json"},
           {"platform_role", "consume external frames; do not generate sensor timing"},
       }},
  };
  writeJson(options_.chain_dir / "operator_initialization.json", initialization);
  std::cout << "[C++ RuntimeHost] 初始化快照已写入" << std::endl;
}

void PlatformRuntimeHost::initializeBranchRegistry() {
  const std::string generated = nowUtcIso();
  runtime_events_ = json::array();
  branch_records_ = json::array({
      {
          {"branch_id", kOnlineFilterBranchId},
          {"branch_kind", "online_mainline"},
          {"display_name", "在线融合主线"},
          {"kind_label", "在线融合"},
          {"branch_roles", json::array({"online_filtering", "fact_state", "posterior_state"})},
          {"parent_branch_id", ""},
          {"workflow_id", online_workflow_id_},
          {"run_id", options_.run_id_prefix + ".online"},
          {"run_dir", pathString(options_.chain_dir)},
          {"status", "running"},
          {"priority", 100},
          {"created_at_utc", generated},
          {"updated_at_utc", generated},
          {"seed_runtime_outputs_ref", ""},
          {"refs",
           {
               {"runtime_host_evidence", "runtime_host_evidence.json"},
               {"run_timeline_index", "run_timeline_index.json"},
               {"sensor_stream", "sensor_stream.json"},
           }},
          {"summary",
           {
               {"frame_count", 0},
               {"step_count", 0},
               {"artifact_count", 0},
               {"qoi_ref_count", 0},
               {"checkpoint_count", 0},
           }},
      },
      {
          {"branch_id", kRealtimePredictionBranchId},
          {"branch_kind", "realtime_prediction"},
          {"display_name", "在线后验场轨（非分支）"},
          {"kind_label", "后验场轨"},
          {"branch_roles", json::array({"online_current_field_reconstruction", "posterior_field_rail"})},
          {"parent_branch_id", kOnlineFilterBranchId},
          {"workflow_id", online_workflow_id_},
          {"run_id", options_.run_id_prefix + ".realtime_prediction"},
          {"run_dir", pathString(options_.chain_dir)},
          {"status", "running"},
          {"priority", 90},
          {"created_at_utc", generated},
          {"updated_at_utc", generated},
          {"seed_runtime_outputs_ref", ""},
          {"refs",
           {
               {"runtime_host_evidence", "runtime_host_evidence.json"},
               {"run_timeline_index", "run_timeline_index.json"},
               {"sensor_stream", "sensor_stream.json"},
           }},
          {"summary",
           {
               {"frame_count", 0},
               {"step_count", 0},
               {"artifact_count", 0},
               {"qoi_ref_count", 0},
               {"checkpoint_count", 0},
           }},
      },
  });
  if (branch_records_.is_array() && branch_records_.size() >= 2) {
    branch_records_[0] = applyBranchTemplate(branch_records_[0], object_runtime_profile_, "online_mainline");
    branch_records_[1] = applyBranchTemplate(branch_records_[1], object_runtime_profile_, "realtime_prediction");
  }
  appendRuntimeEventLocked(
      "online_mainline_started",
      kOnlineFilterBranchId,
      -1,
      0.0,
      {
          {"workflow_id", online_workflow_id_},
          {"external_observation_stream", pathString(options_.external_observation_stream)},
      });
}

json PlatformRuntimeHost::loadExternalFrames() const {
  const json payload = readJson(options_.external_observation_stream);
  json frames = json::array();
  if (payload.is_object() && payload.contains("frames") && payload.at("frames").is_array()) {
    frames = payload.at("frames");
  } else if (payload.is_object() && payload.contains("observations") && payload.at("observations").is_array()) {
    frames = payload.at("observations");
  } else if (payload.is_array()) {
    frames = payload;
  } else if (payload.is_object()) {
    frames.push_back(payload);
  }
  if (!frames.is_array() || frames.empty()) {
    throw std::runtime_error("External observation stream has no frames.");
  }
  return frames;
}

fs::path PlatformRuntimeHost::writeOneFrameStream(const json& frame, int local_index) const {
  const fs::path stream_dir = options_.chain_dir / "external_frames";
  fs::create_directories(stream_dir);
  std::ostringstream name;
  name << "frame_" << std::setw(4) << std::setfill('0') << local_index << ".observation_stream.json";
  const fs::path path = stream_dir / name.str();
  json one = {
      {"schema_version", "flightenv.platform.external_observation_stream_slice.v1"},
      {"source_stream_path", pathString(options_.external_observation_stream)},
      {"slice_index", local_index},
      {"generated_at_utc", nowUtcIso()},
      {"frames", json::array({frame})},
  };
  writeJson(path, one);
  return path;
}

fs::path PlatformRuntimeHost::runOneOnlineFrame(
    const json& frame,
    int local_index,
    const fs::path& previous_seed) {
  const fs::path frame_stream = writeOneFrameStream(frame, local_index);
  std::ostringstream suffix;
  suffix << std::setw(4) << std::setfill('0') << local_index;
  const std::string run_id = options_.run_id_prefix + ".online_frame_" + suffix.str();
  const fs::path run_dir = options_.run_root / online_workflow_id_ / run_id;
  runCompiledWorkflow(
      options_.compiled_online_workflow,
      useNativeBackend() ? &onlineNativeRunner() : nullptr,
      run_dir,
      run_id,
      previous_seed,
      frame_stream,
      1,
      false,
      kOnlineFilterBranchId,
      "online.mainline");
  writeRuntimeBranchIndex(run_dir);
  return run_dir;
}

void PlatformRuntimeHost::writeRuntimeBranchIndex(const fs::path& run_dir) const {
  const json runtime_evidence = objectOrEmpty(readJsonIfExists(run_dir / "runtime_evidence.json"));
  const json runtime_outputs = objectOrEmpty(readJsonIfExists(run_dir / "runtime_outputs.json"));
  const json runtime_loop = objectOrEmpty(readJsonIfExists(run_dir / "runtime_loop_summary.json"));
  const json sensor_stream = objectOrEmpty(readJsonIfExists(run_dir / "sensor_stream.json"));
  const json data_plane = objectOrEmpty(readJsonIfExists(run_dir / "data_plane_manifest.json"));
  const json state_checkpoint = objectOrEmpty(readJsonIfExists(run_dir / "state_checkpoint.json"));

  const std::string run_id =
      jsonString(runtime_evidence, "run_id",
                 jsonString(runtime_outputs, "run_id", jsonString(sensor_stream, "run_id", run_dir.filename().string())));
  const std::string workflow_id =
      jsonString(runtime_evidence, "workflow_id",
                 jsonString(runtime_outputs, "workflow_id", jsonString(sensor_stream, "workflow_id", "")));
  const std::string object_id =
      jsonString(runtime_evidence, "object_id",
                 jsonString(runtime_outputs, "object_id", jsonString(sensor_stream, "object_id", object_id_)));
  const std::string generated = nowUtcIso();
  const json initial_seed = objectOrEmpty(runtime_loop.value("initial_seed", json::object()));
  const std::string seed_runtime_outputs_ref =
      jsonString(initial_seed, "seed_runtime_outputs_path", jsonString(runtime_evidence, "seed_runtime_outputs_ref", ""));
  const std::string branch_kind = inferRuntimeBranchKind(workflow_id, sensor_stream, object_runtime_profile_);
  const std::string branch_id =
      branch_kind == "online_mainline"
          ? kOnlineFilterBranchId
          : (branch_kind == "future_prediction" ? "predict." + safeFileName(run_id) : "single." + safeFileName(run_id));
  const int trigger_frame_index = extractFrameIndexFromSeedPath(seed_runtime_outputs_ref);
  const std::map<int, json> future_labels =
      branch_kind == "future_prediction" ? futureStateLabelsByStep(run_dir, object_runtime_profile_) : std::map<int, json>{};

  json frames = json::array();
  const json source_frames = arrayOrEmpty(sensor_stream.value("frames", json::array()));
  if (source_frames.is_array()) {
    for (auto frame : source_frames) {
      if (!frame.is_object()) {
        continue;
      }
      frame["branch_id"] = branch_id;
      frames.push_back(frame);
    }
  }
  if (frames.empty() && branch_kind == "online_mainline") {
    const json loop_iterations_for_frames = arrayOrEmpty(runtime_loop.value("iterations", json::array()));
    for (auto iteration : loop_iterations_for_frames) {
      if (!iteration.is_object()) {
        continue;
      }
      const int frame_index =
          jsonInt(iteration, "frame_index", jsonInt(iteration, "loop_iteration_index", static_cast<int>(frames.size())));
      const double sample_time_s =
          jsonDouble(iteration, "sample_time_s", jsonDouble(iteration, "public_time_s", static_cast<double>(frame_index)));
      json frame = {
          {"branch_id", branch_id},
          {"frame_index", frame_index},
          {"loop_iteration_index", jsonInt(iteration, "loop_iteration_index", frame_index)},
          {"sample_time_s", sample_time_s},
          {"public_time_s", sample_time_s},
          {"source", "runtime_loop_summary"},
          {"source_runtime_outputs", pathString(run_dir / "runtime_outputs.json")},
          {"posterior_checkpoint", jsonString(iteration, "posterior_checkpoint")},
          {"status", jsonString(iteration, "status", "ok")},
      };
      if (iteration.contains("diagnostics")) {
        frame["diagnostics"] = iteration.at("diagnostics");
      }
      if (iteration.contains("sample_scheduler")) {
        frame["sample_scheduler"] = iteration.at("sample_scheduler");
      }
      frames.push_back(frame);
    }
  }

  json steps = json::array();
  json series_by_id = json::object();
  const json iterations = arrayOrEmpty(runtime_loop.value("iterations", json::array()));
  if (iterations.is_array()) {
    for (auto iteration : iterations) {
      if (!iteration.is_object()) {
        continue;
      }
      iteration = RuntimeTimelineMaterializer::makeBranchStep(iteration, branch_id, static_cast<int>(steps.size()));
      const int step_index = jsonInt(iteration, "step_index", static_cast<int>(steps.size()));
      const double time_s = jsonDouble(iteration, "public_time_s", static_cast<double>(step_index));
      auto label_it = future_labels.find(step_index);
      if (label_it != future_labels.end()) {
        applyStateLabel(iteration, label_it->second);
      }
      steps.push_back(iteration);
      for (auto it = iteration.begin(); it != iteration.end(); ++it) {
        if (!it.value().is_number()) {
          continue;
        }
        const std::string key = it.key();
        if (key == "iteration_index" || key == "loop_iteration_index" || key == "step_index") {
          continue;
        }
        addSeriesPoint(series_by_id,
                       "step." + key,
                       "step." + key,
                       it.value().get<double>(),
                       time_s,
                       "runtime_loop_summary.json",
                       branch_id,
                       "branch_step",
                       step_index);
      }
    }
  }
  json artifact_refs = json::array();
  json qoi_refs = json::array();
  const json entries = arrayOrEmpty(data_plane.value("entries", json::array()));
  if (entries.is_array()) {
    for (auto entry : entries) {
      if (!entry.is_object() || jsonString(entry, "direction", "output") != "output") {
        continue;
      }
      entry = RuntimeTimelineMaterializer::makeArtifactRef(entry, branch_id);
      const int step_index = jsonInt(entry, "step_index", jsonInt(entry, "loop_iteration_index", -1));
      if (step_index >= 0) {
        auto label_it = future_labels.find(step_index);
        if (label_it != future_labels.end()) {
          applyStateLabel(entry, label_it->second);
        }
      }
      artifact_refs.push_back(entry);
      if (RuntimeTimelineMaterializer::isQoiRef(entry)) {
        qoi_refs.push_back(entry);
      }
      const json time_point = objectOrEmpty(entry.value("time_point", json::object()));
      const double time_s = jsonDouble(entry,
                                       "public_time_s",
                                       jsonDouble(time_point,
                                                  "run_time_s",
                                                  step_index >= 0 ? static_cast<double>(step_index) : 0.0));
      const json stats = objectOrEmpty(entry.value("statistics", json::object()));
      if (stats.is_object()) {
        for (auto it = stats.begin(); it != stats.end(); ++it) {
          if (!it.value().is_number()) {
            continue;
          }
          const std::string port = jsonString(entry, "port_id", "output");
          const std::string stat = it.key();
          addSeriesPoint(series_by_id,
                         "artifact." + port + "." + stat,
                         port + "." + stat,
                         it.value().get<double>(),
                         time_s,
                         "data_plane_manifest.json",
                         branch_id,
                         "artifact_statistics",
                         step_index,
                         jsonString(entry, "unit"));
        }
      }
    }
  }
  json checkpoint_refs = json::array();
  const json checkpoints = arrayOrEmpty(state_checkpoint.value("checkpoints", json::array()));
  if (checkpoints.is_array()) {
    for (auto checkpoint : checkpoints) {
      if (!checkpoint.is_object()) {
        continue;
      }
      checkpoint["branch_id"] = branch_id;
      checkpoint_refs.push_back(checkpoint);
    }
  }
  const std::string status = jsonString(runtime_evidence, "status", "unknown");
  json branch_summary = {
      {"frame_count", frames.size()},
      {"step_count", steps.size()},
      {"artifact_count", artifact_refs.size()},
      {"qoi_ref_count", qoi_refs.size()},
      {"checkpoint_count", checkpoint_refs.size()},
  };
  if (runtime_loop.contains("summary")) {
    branch_summary["loop_summary"] = objectOrEmpty(runtime_loop.value("summary", json::object()));
  }

  json branch = {
      {"branch_id", branch_id},
      {"branch_kind", branch_kind},
      {"parent_branch_id",
       branch_kind == "future_prediction"
           ? branchParentFromProfile(object_runtime_profile_, "future_prediction", kRealtimePredictionBranchId)
           : ""},
      {"workflow_id", workflow_id},
      {"run_id", run_id},
      {"run_dir", pathString(run_dir)},
      {"status", status},
      {"priority", branch_kind == "online_mainline" ? 100 : 10},
      {"created_at_utc", generated},
      {"updated_at_utc", generated},
      {"seed_runtime_outputs_ref", seed_runtime_outputs_ref},
      {"refs",
       {
           {"runtime_evidence", "runtime_evidence.json"},
           {"runtime_outputs", "runtime_outputs.json"},
           {"runtime_loop_summary", "runtime_loop_summary.json"},
           {"sensor_stream", "sensor_stream.json"},
           {"data_plane_manifest", "data_plane_manifest.json"},
           {"state_checkpoint", "state_checkpoint.json"},
       }},
      {"summary", branch_summary},
  };
  if (branch_kind == "future_prediction") {
    branch = applyBranchTemplate(branch, object_runtime_profile_, "future_prediction");
  } else if (branch_kind == "online_mainline") {
    branch = applyBranchTemplate(branch, object_runtime_profile_, "online_mainline");
  }
  if (trigger_frame_index >= 0) {
    branch["trigger_frame_index"] = trigger_frame_index;
  }

  json cursor = {
      {"schema_version", "flightenv.platform.runtime_cursor.v1"},
      {"cursor_id", "ui.active"},
      {"run_id", run_id},
      {"object_id", object_id},
      {"generated_at_utc", generated},
      {"mode", branch_kind == "online_mainline" ? "mainline_replay_frame" : "prediction_branch_replay"},
      {"branch_id", branch_id},
      {"follow_live", false},
      {"status", status},
      {"refs", {{"run_timeline_index", "run_timeline_index.json"}}},
  };
  if (!frames.empty() && frames.back().is_object()) {
    cursor["frame_index"] = jsonInt(frames.back(), "frame_index", static_cast<int>(frames.size()) - 1);
    cursor["time_s"] = jsonDouble(frames.back(), "sample_time_s", jsonDouble(frames.back(), "runtime_time_s", 0.0));
  }
  if (!steps.empty() && steps.back().is_object()) {
    cursor["step_index"] = jsonInt(steps.back(), "step_index", static_cast<int>(steps.size()) - 1);
    cursor["time_s"] = jsonDouble(steps.back(), "run_time_s", jsonDouble(cursor, "time_s", 0.0));
  }

  json series = json::array();
  for (auto it = series_by_id.begin(); it != series_by_id.end(); ++it) {
    series.push_back(it.value());
  }
  writeJson(run_dir / "branch_registry.json",
            {
                {"schema_version", "flightenv.platform.branch_registry.v1"},
                {"run_id", run_id},
                {"workflow_id", workflow_id},
                {"object_id", object_id},
                {"generated_at_utc", generated},
                {"primary_branch_id", branch_id},
                {"branches", json::array({branch})},
                {"summary",
                 {
                     {"branch_count", 1},
                     {"online_branch_count", branch_kind == "online_mainline" ? 1 : 0},
                     {"prediction_branch_count", branch_kind == "future_prediction" ? 1 : 0},
                 }},
            });
  writeJson(run_dir / "runtime_cursor.json", cursor);
  writeJson(run_dir / "series_manifest.json",
            {
                {"schema_version", "flightenv.platform.series_manifest.v1"},
                {"run_id", run_id},
                {"workflow_id", workflow_id},
                {"object_id", object_id},
                {"branch_id", branch_id},
                {"generated_at_utc", generated},
                {"default_x_axis", "time_s"},
                {"series", series},
                {"summary", {{"series_count", series.size()}}},
            });
  writeJson(run_dir / "run_timeline_index.json",
            {
                {"schema_version", "flightenv.platform.run_timeline_index.v1"},
                {"run_id", run_id},
                {"workflow_id", workflow_id},
                {"object_id", object_id},
                {"generated_at_utc", generated},
                {"source_root", pathString(run_dir)},
                {"branch_registry_ref", "branch_registry.json"},
                {"cursor_ref", "runtime_cursor.json"},
                {"branches", json::array({branch})},
                {"online_frames", frames},
                {"branch_steps", steps},
                {"artifact_refs", artifact_refs},
                {"qoi_refs", qoi_refs},
                {"checkpoint_refs", checkpoint_refs},
                {"series_manifest_refs", json::array({{{"branch_id", branch_id}, {"ref", "series_manifest.json"}}})},
                {"summary",
                 {
                     {"branch_count", 1},
                     {"online_frame_count", frames.size()},
                     {"branch_step_count", steps.size()},
                     {"artifact_ref_count", artifact_refs.size()},
                     {"qoi_ref_count", qoi_refs.size()},
                     {"series_count", series.size()},
                 }},
            });
}

void PlatformRuntimeHost::mergeSeriesManifest(const fs::path& manifest_path, const std::string& branch_id) {
  const json manifest = readJsonIfExists(manifest_path);
  if (!manifest.is_object() || !manifest.contains("series") || !manifest.at("series").is_array()) {
    return;
  }
  for (const auto& item : manifest.at("series")) {
    if (!item.is_object()) {
      continue;
    }
    const std::string source_id = jsonString(item, "series_id", "");
    if (source_id.empty()) {
      continue;
    }
    const std::string combined_id = branch_id == kOnlineFilterBranchId ? source_id : branch_id + "." + source_id;
    if (!series_by_id_.contains(combined_id)) {
      json copy = item;
      copy["series_id"] = combined_id;
      copy["branch_id"] = branch_id;
      copy["points"] = json::array();
      series_by_id_[combined_id] = copy;
    }
    if (item.contains("points") && item.at("points").is_array()) {
      for (auto point : item.at("points")) {
        if (point.is_object()) {
          point["branch_id"] = branch_id;
          series_by_id_[combined_id]["points"].push_back(point);
        }
      }
    }
  }
}

void PlatformRuntimeHost::appendRuntimeIndex(
    const fs::path& run_dir,
    const std::string& branch_id,
    bool online_branch,
    int mainline_frame_index,
    double mainline_time_origin_s) {
  const json timeline = readJsonIfExists(run_dir / "run_timeline_index.json");
  const std::map<int, json> future_step_labels =
      (!online_branch && isFuturePredictionBranchId(branch_id))
          ? futureStateLabelsByStep(run_dir, object_runtime_profile_)
          : std::map<int, json>{};
  if (timeline.is_object()) {
    const auto normalize_item = [&](json& item, const std::string& target_branch_id) {
      const int source_loop =
          jsonInt(item, "loop_iteration_index", jsonInt(item, "step_index", -1));
      item["branch_id"] = target_branch_id;
      item["source_run_dir"] = pathString(run_dir);
      item["source_runtime_outputs"] = pathString(run_dir / "runtime_outputs.json");
      if (mainline_frame_index >= 0) {
        item["mainline_frame_index"] = mainline_frame_index;
      }
      if (online_branch && mainline_frame_index >= 0) {
        if (source_loop >= 0) {
          item["source_loop_iteration_index"] = source_loop;
          item["source_step_index"] = jsonInt(item, "step_index", source_loop);
        }
        item["frame_index"] = mainline_frame_index;
        item["loop_iteration_index"] = mainline_frame_index;
        item["step_index"] = mainline_frame_index;
        forceMainlinePublicTime(item, mainline_time_origin_s, mainline_frame_index);
      }
      if (online_branch && target_branch_id == kRealtimePredictionBranchId) {
        item["source_online_branch_id"] = kOnlineFilterBranchId;
        item["frame_role"] = "realtime_prediction_frame";
        if (!item.contains("step_role") || !item.at("step_role").is_string()) {
          item["step_role"] = "online_operator_step";
        }
        if (!item.contains("stage_id") || !item.at("stage_id").is_string()) {
          item["stage_id"] = "online_operator_step";
        }
      }
      if (!online_branch && isFuturePredictionBranchId(target_branch_id)) {
        if (std::isfinite(mainline_time_origin_s)) {
          item["branch_relative_time_s"] = publicTimeFromItem(item, 0.0);
          item["trigger_time_s"] = mainline_time_origin_s;
          item["mainline_time_origin_s"] = mainline_time_origin_s;
          offsetNestedTime(item, mainline_time_origin_s, mainline_frame_index);
        }
        const int label_index =
            jsonInt(item, "source_loop_iteration_index",
                    jsonInt(item, "loop_iteration_index", jsonInt(item, "step_index", -1)));
        const auto label_it = future_step_labels.find(label_index);
        if (label_it != future_step_labels.end()) {
          applyStateLabel(item, label_it->second);
        }
        item["frame_role"] = "future_prediction_frame";
        item["future_frame_index"] = label_index;
        if (!item.contains("display_role") || !item.at("display_role").is_string() ||
            item.at("display_role").get<std::string>().empty()) {
          const std::string role = displayRoleForPredictionArtifact(item, object_runtime_profile_);
          if (!role.empty()) {
            item["display_role"] = role;
          }
        }
      }
    };

    const auto append_items = [&](const char* key, json& target) {
      if (!timeline.contains(key) || !timeline.at(key).is_array()) {
        return;
      }
      for (auto item : timeline.at(key)) {
        if (!item.is_object()) {
          continue;
        }
        const std::string target_branch_id =
            online_branch ? std::string(kRealtimePredictionBranchId) : branch_id;
        normalize_item(item, target_branch_id);
        if (online_branch && std::string(key) == "artifact_refs" &&
            isRealtimePredictionFieldArtifact(item, object_runtime_profile_)) {
          item["display_role"] = "realtime_prediction.current_field";
          item["branch_view"] = "realtime_prediction";
        }
        target.push_back(item);
      }
    };
    append_items("branch_steps", branch_steps_);
    append_items("artifact_refs", artifact_refs_);
    append_items("qoi_refs", qoi_refs_);
    append_items("checkpoint_refs", checkpoint_refs_);
    if (online_branch && timeline.contains("online_frames") && timeline.at("online_frames").is_array()) {
      for (auto frame : timeline.at("online_frames")) {
        if (!frame.is_object()) {
          continue;
        }
        frame["branch_id"] = branch_id;
        frame["source_run_dir"] = pathString(run_dir);
        frame["source_runtime_outputs"] = pathString(run_dir / "runtime_outputs.json");
        frame["mainline_frame_index"] = mainline_frame_index;
        frame["frame_index"] = mainline_frame_index;
        frame["loop_iteration_index"] = mainline_frame_index;
        forceMainlinePublicTime(
            frame,
            jsonDouble(frame, "sample_time_s", mainline_time_origin_s),
            mainline_frame_index);
        frame["frame_role"] = "online_fusion_frame";
        frame["display_role"] = "online_filter.fusion_frame";
        frame["realtime_prediction_branch_id"] = kRealtimePredictionBranchId;
        applyStateLabel(frame, stateLabelFromOnlineFrame(frame, object_runtime_profile_));
        online_frames_.push_back(frame);
      }
    }
  }
  mergeSeriesManifest(run_dir / "series_manifest.json",
                      online_branch ? std::string(kRealtimePredictionBranchId) : branch_id);
}

void PlatformRuntimeHost::upsertBranchRecord(const json& record) {
  RuntimeBranchService::upsertRecord(branch_records_, record);
}

void PlatformRuntimeHost::updateBranchStatus(
    const std::string& branch_id,
    const std::string& status,
    const json& summary) {
  StateLock lock(*this);
  RuntimeBranchService::updateRecordStatus(branch_records_, branch_id, status, summary);
  appendRuntimeEventLocked(
      "prediction_branch_status_changed",
      branch_id,
      -1,
      current_source_time_s_,
      {{"status", status}, {"summary", summary.is_null() ? json::object() : summary}});
  writeRuntimeIndexesLocked(status == "running" ? "prediction_branch_live" : "live_tail");
  writeProgressLocked();
}

std::string PlatformRuntimeHost::appendRuntimeEventLocked(
    const std::string& event_kind,
    const std::string& branch_id,
    int frame_index,
    double time_s,
    const json& payload) {
  return RuntimeBranchService::appendEvent(
      runtime_events_,
      options_.run_id_prefix,
      event_kind,
      branch_id,
      frame_index,
      time_s,
      payload);
}

RuntimeEvent PlatformRuntimeHost::dispatchRuntimeEventLocked(RuntimeEvent event) {
  runtime_event_loop_.push(std::move(event));
  return runtime_event_loop_.next();
}

void PlatformRuntimeHost::maybeForkPredictionBranch(
    int local_index,
    const json& frame,
    const fs::path& online_runtime_outputs,
    const std::string& posterior_event_id) {
  if (!automatic_branching_enabled_) {
    return;
  }
  bool by_interval = false;
  if (branch_trigger_kind_ == "every_n_frames") {
    by_interval = effective_prediction_every_frames_ > 0 &&
                  ((local_index + 1) % effective_prediction_every_frames_) == 0;
  } else if (branch_trigger_kind_ == "time_interval") {
    const double sample_time = jsonDouble(frame, "sample_time_s", jsonDouble(frame, "time_s", local_index));
    by_interval = effective_prediction_time_interval_s_ > 0.0 &&
                  (!has_prediction_trigger_time_ ||
                   sample_time - last_prediction_trigger_time_s_ >= effective_prediction_time_interval_s_);
    if (by_interval) {
      last_prediction_trigger_time_s_ = sample_time;
      has_prediction_trigger_time_ = true;
    }
  } else if (branch_trigger_kind_ == "on_event") {
    by_interval = jsonBool(frame, "trigger_prediction_branch", false);
  }
  if (!by_interval) {
    return;
  }

  int active_branch_count = static_cast<int>(branch_threads_.size());
  if (options_.branch_manager_enabled) {
    active_branch_count = 0;
    for (const auto& branch : branch_records_) {
      if (jsonString(branch, "branch_kind", "") != "future_prediction") {
        continue;
      }
      const std::string status = jsonString(branch, "status", "");
      if (status == "queued" || status == "running" || status == "stop_requested") {
        ++active_branch_count;
      }
    }
  }

  if (effective_max_concurrent_branches_ > 0 &&
      active_branch_count >= effective_max_concurrent_branches_) {
    StateLock lock(*this);
    current_message_ = "预测分支达到对象声明的最大并发数，本次触发只记录不阻塞主线";
    appendRuntimeEventLocked(
        "prediction_branch_capacity_full",
        kRealtimePredictionBranchId,
        local_index,
        jsonDouble(frame, "sample_time_s", jsonDouble(frame, "time_s", local_index)),
        {
            {"trigger_event_id", posterior_event_id},
            {"max_concurrent_branches", effective_max_concurrent_branches_},
            {"active_branch_tasks", active_branch_count},
            {"policy_source", "object_workflow.branching_policy"},
        });
    writeRuntimeIndexesLocked("live_tail");
    writeProgressLocked();
    return;
  }

  std::ostringstream frame_tag;
  frame_tag << std::setw(4) << std::setfill('0') << local_index;
  std::ostringstream branch_id_stream;
  branch_id_stream << "predict.frame_" << frame_tag.str() << "." << std::setw(3)
                   << std::setfill('0') << requested_prediction_runs_;
  const std::string branch_id = branch_id_stream.str();
  const std::string run_id = options_.run_id_prefix + ".predict_frame_" + frame_tag.str();
  const fs::path run_dir = options_.run_root / future_workflow_id_ / run_id;
  const double trigger_time = jsonDouble(frame, "sample_time_s", jsonDouble(frame, "time_s", local_index));
  const RuntimeEvent branch_trigger_event = RuntimeEventQueue::branchTriggeredEvent(
      branch_id,
      trigger_time,
      local_index,
      posterior_event_id,
      {
          {"source_event_id", posterior_event_id},
          {"source_event_kind", "posterior_frame_committed"},
          {"parent_branch_id", kRealtimePredictionBranchId},
          {"workflow_id", future_workflow_id_},
          {"run_id", run_id},
          {"run_dir", pathString(run_dir)},
          {"seed_runtime_outputs_ref", pathString(online_runtime_outputs)},
      });

  PredictionTask task;
  task.branch_id = branch_id;
  task.run_id = run_id;
  task.run_dir = run_dir;
  task.seed_runtime_outputs = online_runtime_outputs;
  task.trigger_frame_index = local_index;
  task.trigger_time_s = trigger_time;
  task.trigger_event_id = branch_trigger_event.event_id;

  {
    StateLock lock(*this);
    const RuntimeEvent dispatched_branch_event = dispatchRuntimeEventLocked(branch_trigger_event);
    ++requested_prediction_runs_;
    latest_prediction_branch_id_ = branch_id;
    for (auto& item : branch_records_) {
      if (jsonString(item, "branch_kind", "") != "future_prediction") {
        continue;
      }
      item["is_realtime_prediction"] = false;
      item["kind_label"] = "历史预测";
      const int trigger = jsonInt(item, "trigger_frame_index", -1);
      item["display_name"] = trigger >= 0
                                 ? "历史预测分支 · 帧 " + std::to_string(trigger)
                                 : "历史预测分支 · " + jsonString(item, "branch_id", "");
      item["branch_roles"] = json::array({"future_prediction_snapshot"});
    }
    std::cout << "[C++ RuntimeHost] 登记预测分支 " << branch_id << std::endl;
    json branch_record = {
        {"branch_id", branch_id},
        {"branch_kind", "future_prediction"},
        {"display_name", "未来预测分支 · 源帧 " + std::to_string(local_index)},
        {"kind_label", "未来预测"},
        {"branch_roles", json::array({"future_prediction", "future_prediction_snapshot"})},
        {"is_realtime_prediction", true},
        {"parent_branch_id", branchParentFromProfile(object_runtime_profile_, "future_prediction", kRealtimePredictionBranchId)},
        {"workflow_id", future_workflow_id_},
        {"run_id", run_id},
        {"run_dir", pathString(run_dir)},
        {"status", "queued"},
        {"priority", 10},
        {"created_at_utc", nowUtcIso()},
        {"updated_at_utc", nowUtcIso()},
        {"trigger_frame_index", local_index},
        {"trigger_time_s", trigger_time},
        {"trigger_event_id", dispatched_branch_event.event_id},
        {"trigger_event_kind", dispatched_branch_event.event_kind},
        {"trigger_cause_event_id", posterior_event_id},
        {"seed_runtime_outputs_ref", pathString(online_runtime_outputs)},
        {"refs",
         {
             {"runtime_evidence", "runtime_evidence.json"},
             {"runtime_outputs", "runtime_outputs.json"},
             {"runtime_loop_summary", "runtime_loop_summary.json"},
             {"data_plane_manifest", "data_plane_manifest.json"},
             {"state_checkpoint", "state_checkpoint.json"},
             {"branch_manager_state", pathString(branchManagerStatePath(options_.chain_dir, branch_id))},
             {"branch_control", pathString(branchControlPath(options_.chain_dir, branch_id))},
         }},
        {"summary", {{"iteration_count", 0}, {"field_artifact_count", 0}}},
    };
    branch_record = applyBranchTemplate(branch_record, object_runtime_profile_, "future_prediction");
    const std::string base_display =
        jsonString(branchTemplate(object_runtime_profile_, "future_prediction"),
                   "display_name",
                   jsonString(branch_record, "display_name", "future prediction branch"));
    const std::string source_frame_label =
        jsonString(branchTemplate(object_runtime_profile_, "future_prediction"),
                   "source_frame_label",
                   "source frame");
    branch_record["display_name"] = base_display + " · " + source_frame_label + " " + std::to_string(local_index);
    upsertBranchRecord(branch_record);
    appendRuntimeEventLocked(
        dispatched_branch_event.event_kind,
        branch_id,
        local_index,
        trigger_time,
        RuntimeEventQueue::eventEvidence(dispatched_branch_event));
    current_message_ = "已从在线帧 " + std::to_string(local_index) + " 分叉预测分支";
    appendRuntimeEventLocked(
        "prediction_branch_queued",
        branch_id,
        local_index,
        trigger_time,
        {
            {"trigger_event_id", dispatched_branch_event.event_id},
            {"trigger_event_kind", dispatched_branch_event.event_kind},
            {"trigger_cause_event_id", posterior_event_id},
            {"run_id", run_id},
            {"run_dir", pathString(run_dir)},
            {"seed_runtime_outputs_ref", pathString(online_runtime_outputs)},
            {"workflow_id", future_workflow_id_},
        });
    writeRuntimeIndexesLocked("live_tail");
    writeProgressLocked();
  }

  if (options_.branch_manager_enabled) {
    launchPredictionBranchWorker(task);
  } else {
    branch_threads_.emplace_back([this, task]() {
      runPredictionBranch(task);
    });
  }
  std::cout << "[C++ RuntimeHost] 预测分支已提交 " << branch_id << std::endl;
}

void PlatformRuntimeHost::launchPredictionBranchWorker(const PredictionTask& task) {
  fs::create_directories(options_.chain_dir / "branch_controls");
  fs::create_directories(options_.chain_dir / "branch_manager");
  const fs::path control_path = branchControlPath(options_.chain_dir, task.branch_id);
  const fs::path state_path = branchManagerStatePath(options_.chain_dir, task.branch_id);
  const fs::path log_path = options_.chain_dir / "branch_worker_logs" / (safeFileName(task.branch_id) + ".log");

  std::vector<std::string> args = {
      pathString(options_.runtime_host_exe),
      "--branch-worker",
      "--workspace-root", pathString(options_.workspace_root),
      "--pdk-root", pathString(options_.pdk_root),
      "--object-package-root", pathString(options_.object_package_root),
      "--compiled-online", pathString(options_.compiled_online_workflow),
      "--compiled-future", pathString(options_.compiled_future_workflow),
      "--adapter-registry", pathString(options_.adapter_registry),
      "--pdk-cli", pathString(options_.pdk_cli),
      "--pdk-run-script", pathString(options_.pdk_run_script),
      "--run-root", pathString(options_.run_root),
      "--chain-dir", pathString(options_.chain_dir),
      "--python", options_.python,
      "--execution-backend", options_.execution_backend,
      "--zero-copy-mode", options_.runtime_zero_copy_mode,
      "--typed-buffer-persistence", options_.typed_buffer_persistence,
      "--run-id-prefix", options_.run_id_prefix,
      "--future-max-iterations", std::to_string(options_.future_max_iterations),
      "--branch-chunk-iterations", std::to_string(options_.branch_chunk_iterations),
      "--branch-id", task.branch_id,
      "--branch-run-id", task.run_id,
      "--branch-run-dir", pathString(task.run_dir),
      "--seed-runtime-outputs", pathString(task.seed_runtime_outputs),
      "--trigger-frame-index", std::to_string(task.trigger_frame_index),
      "--trigger-time-s", std::to_string(task.trigger_time_s),
      "--trigger-event-id", task.trigger_event_id,
  };
  if (!options_.require_adapter_registry) {
    args.push_back("--no-require-adapter-registry");
  }
  if (options_.allow_legacy_process_backend) {
    args.push_back("--allow-legacy-process-backend");
  }

  writeJson(control_path,
            {
                {"schema_version", "flightenv.platform.branch_control.v1"},
                {"branch_id", task.branch_id},
                {"command", "run"},
                {"requested_at_utc", nowUtcIso()},
            });
  writeJson(state_path,
            {
                {"schema_version", "flightenv.platform.branch_manager_state.v1"},
                {"branch_id", task.branch_id},
                {"run_id", task.run_id},
                {"run_dir", pathString(task.run_dir)},
                {"status", "queued"},
                {"target_iterations", options_.future_max_iterations},
                {"chunk_iterations", options_.branch_chunk_iterations},
                {"completed_iterations", 0},
                {"next_chunk_index", 0},
                {"latest_seed_runtime_outputs", pathString(task.seed_runtime_outputs)},
                {"trigger_frame_index", task.trigger_frame_index},
                {"trigger_time_s", task.trigger_time_s},
                {"trigger_event_id", task.trigger_event_id},
                {"control_ref", pathString(control_path)},
                {"worker_log_ref", pathString(log_path)},
                {"created_at_utc", nowUtcIso()},
                {"updated_at_utc", nowUtcIso()},
                {"worker_args", args},
            });

  unsigned long pid = 0;
  if (!launchDetachedProcess(args, options_.workspace_root, log_path, &pid)) {
    updateBranchStatus(task.branch_id, "failed",
                       {{"reason", "failed_to_launch_branch_worker"},
                        {"runtime_host_exe", pathString(options_.runtime_host_exe)}});
    return;
  }

  updateBranchStatus(task.branch_id, "running",
                     {
                         {"manager", "background_branch_worker"},
                         {"process_id", pid},
                         {"state_ref", pathString(state_path)},
                         {"control_ref", pathString(control_path)},
                         {"worker_log_ref", pathString(log_path)},
                         {"target_iterations", options_.future_max_iterations},
                         {"chunk_iterations", options_.branch_chunk_iterations},
                         {"completed_iterations", 0},
                     });
}

void PlatformRuntimeHost::runPredictionBranch(PredictionTask task) {
  try {
    std::cout << "[C++ RuntimeHost] 预测分支启动 " << task.branch_id << std::endl;
    updateBranchStatus(task.branch_id, "running", json::object());
    std::cout << "[C++ RuntimeHost] 预测分支状态已更新 running " << task.branch_id << std::endl;
    runCompiledWorkflow(
        options_.compiled_future_workflow,
        nullptr,
        task.run_dir,
        task.run_id,
        task.seed_runtime_outputs,
        {},
        options_.future_max_iterations,
        false,
        task.branch_id,
        "future.prediction");
    std::cout << "[C++ RuntimeHost] 预测分支 workflow 完成 " << task.branch_id << std::endl;
    writeRuntimeBranchIndex(task.run_dir);
    std::cout << "[C++ RuntimeHost] 预测分支索引完成 " << task.branch_id << std::endl;

    const json loop = readJson(task.run_dir / "runtime_loop_summary.json");
    const json outputs = readJson(task.run_dir / "runtime_outputs.json");
    const json evidence = readJson(task.run_dir / "runtime_evidence.json");
    const int iteration_count = jsonInt(loop.value("summary", json::object()), "iteration_count", 0);
    const int field_count = countFieldArtifacts(task.run_dir);
    const json prediction_metrics =
        collectConfiguredMetrics(json::object(), outputs, object_runtime_profile_, "prediction_summary_metrics");
    json last_iteration = json::object();
    if (loop.contains("iterations") && loop.at("iterations").is_array() && !loop.at("iterations").empty()) {
      last_iteration = loop.at("iterations").back();
    }
    const json last_altitude =
        last_iteration.contains("altitude_m") ? last_iteration.at("altitude_m") : json(nullptr);

    json prediction = {
        {"trigger_frame_index", task.trigger_frame_index},
        {"trigger_time_s", task.trigger_time_s},
        {"trigger_event_id", task.trigger_event_id},
        {"seed_runtime_outputs", pathString(task.seed_runtime_outputs)},
        {"branch_id", task.branch_id},
        {"run_id", task.run_id},
        {"run_dir", pathString(task.run_dir)},
        {"status", jsonString(evidence, "status", "ok")},
        {"iteration_count", iteration_count},
        {"stop_reason", jsonString(loop.value("summary", json::object()), "stop_reason", "")},
        {"stopped", jsonBool(loop.value("summary", json::object()), "stopped", false)},
        {"last_altitude_m", last_altitude},
        {"field_artifact_count", field_count},
    };
    for (auto it = prediction_metrics.begin(); it != prediction_metrics.end(); ++it) {
      prediction[it.key()] = it.value();
    }

    json summary = {
        {"iteration_count", iteration_count},
        {"stop_reason", prediction.value("stop_reason", "")},
        {"field_artifact_count", field_count},
        {"last_altitude_m", last_altitude},
    };
    for (auto it = prediction_metrics.begin(); it != prediction_metrics.end(); ++it) {
      summary[it.key()] = it.value();
    }

    {
      StateLock lock(*this);
      ++completed_prediction_runs_;
      prediction_runs_.push_back(prediction);
      appendRuntimeIndex(task.run_dir, task.branch_id, false, task.trigger_frame_index, task.trigger_time_s);
      appendRuntimeEventLocked(
          "prediction_branch_completed",
          task.branch_id,
          task.trigger_frame_index,
          task.trigger_time_s,
          {
              {"trigger_event_id", task.trigger_event_id},
              {"run_id", task.run_id},
              {"run_dir", pathString(task.run_dir)},
              {"iteration_count", iteration_count},
              {"field_artifact_count", field_count},
              {"status", jsonString(evidence, "status", "ok")},
          });
      for (auto& item : branch_records_) {
        if (jsonString(item, "branch_id", "") == task.branch_id) {
          item["status"] = jsonString(evidence, "status", "ok");
          item["updated_at_utc"] = nowUtcIso();
          item["summary"] = summary;
          break;
        }
      }
      current_message_ = "预测分支完成: " + task.branch_id;
      writeRuntimeIndexesLocked("live_tail");
      writeProgressLocked();
    }
    std::cout << "[C++ RuntimeHost] 预测分支收口完成 " << task.branch_id << std::endl;
  } catch (const std::exception& exc) {
    std::cerr << "[WARN] 预测分支失败: " << task.branch_id << " reason=" << exc.what() << std::endl;
    {
      StateLock lock(*this);
      ++failed_prediction_runs_;
      prediction_runs_.push_back({
          {"trigger_frame_index", task.trigger_frame_index},
          {"trigger_event_id", task.trigger_event_id},
          {"branch_id", task.branch_id},
          {"run_id", task.run_id},
          {"run_dir", pathString(task.run_dir)},
          {"status", "failed"},
          {"reason", exc.what()},
      });
      appendRuntimeEventLocked(
          "prediction_branch_failed",
          task.branch_id,
          task.trigger_frame_index,
          task.trigger_time_s,
          {
              {"trigger_event_id", task.trigger_event_id},
              {"run_id", task.run_id},
              {"run_dir", pathString(task.run_dir)},
              {"reason", exc.what()},
          });
      for (auto& item : branch_records_) {
        if (jsonString(item, "branch_id", "") == task.branch_id) {
          item["status"] = "failed";
          item["updated_at_utc"] = nowUtcIso();
          item["summary"] = {{"reason", exc.what()}};
          break;
        }
      }
      current_message_ = std::string("预测分支失败但主线不崩溃: ") + exc.what();
      writeRuntimeIndexesLocked("live_tail");
      writeProgressLocked();
    }
  }
}

int PlatformRuntimeHost::runBranchWorker() {
  try {
    ensureInputs();
    loadCompiledWorkflowMetadata();
    if (options_.branch_id.empty()) {
      throw std::runtime_error("--branch-id is required for --branch-worker");
    }
    fs::create_directories(options_.chain_dir / "branch_controls");
    fs::create_directories(options_.chain_dir / "branch_manager");

    const fs::path state_path = branchManagerStatePath(options_.chain_dir, options_.branch_id);
    const fs::path control_path = branchControlPath(options_.chain_dir, options_.branch_id);
    json state = readJsonIfExists(state_path);
    if (options_.branch_run_id.empty()) {
      options_.branch_run_id = jsonString(state, "run_id", "");
    }
    if (options_.branch_run_id.empty()) {
      options_.branch_run_id = options_.run_id_prefix + "." + safeFileName(options_.branch_id);
    }
    if (options_.branch_run_dir.empty()) {
      const std::string state_run_dir = jsonString(state, "run_dir", "");
      if (!state_run_dir.empty()) {
        options_.branch_run_dir = state_run_dir;
      }
    }
    if (options_.branch_run_dir.empty()) {
      options_.branch_run_dir = options_.run_root / future_workflow_id_ / options_.branch_run_id;
    }
    options_.branch_run_dir = fs::absolute(options_.branch_run_dir);
    fs::create_directories(options_.branch_run_dir);
    const fs::path chunks_root = options_.branch_run_dir / "_branch_chunks";
    fs::create_directories(chunks_root);

    json preserved_worker_args = json::array();
    if (state.is_object() && state.contains("worker_args") && state.at("worker_args").is_array()) {
      preserved_worker_args = state.at("worker_args");
    }
    int completed_iterations = options_.resume_existing_branch ? jsonInt(state, "completed_iterations", 0) : 0;
    int chunk_index = options_.resume_existing_branch ? jsonInt(state, "next_chunk_index", 0) : 0;
    fs::path latest_seed = options_.resume_existing_branch
                               ? fs::path(jsonString(state, "latest_seed_runtime_outputs", ""))
                               : options_.seed_runtime_outputs;
    if (latest_seed.empty()) {
      latest_seed = options_.seed_runtime_outputs;
    }
    if (latest_seed.empty() || !fs::exists(latest_seed)) {
      throw std::runtime_error("Prediction branch seed runtime_outputs not found: " + pathString(latest_seed));
    }

    const int target_iterations =
        options_.resume_existing_branch
            ? std::max(std::max(1, jsonInt(state, "target_iterations", options_.future_max_iterations)),
                       std::max(1, options_.future_max_iterations))
            : std::max(1, options_.future_max_iterations);
    const int chunk_iterations =
        options_.resume_existing_branch
            ? std::max(1, jsonInt(state, "chunk_iterations", options_.branch_chunk_iterations))
            : std::max(1, options_.branch_chunk_iterations);
    const double base_dt_s = workflowBaseDtS(future_workflow_snapshot_);
    const double output_period_s = workflowOutputPeriodS(future_workflow_snapshot_);
    const std::string created_at = jsonString(state, "created_at_utc", nowUtcIso());

    json aggregate_loop = options_.resume_existing_branch
                              ? readJsonIfExists(options_.branch_run_dir / "runtime_loop_summary.json")
                              : json::object();
    json aggregate_data = options_.resume_existing_branch
                              ? readJsonIfExists(options_.branch_run_dir / "data_plane_manifest.json")
                              : json::object();
    json aggregate_checkpoint = options_.resume_existing_branch
                                    ? readJsonIfExists(options_.branch_run_dir / "state_checkpoint.json")
                                    : json::object();
    json aggregate_nodes = options_.resume_existing_branch
                               ? readJsonIfExists(options_.branch_run_dir / "runtime_node_snapshot.json")
                               : json::object();

    aggregate_loop["schema_version"] = "flightenv.platform.runtime_loop_summary.v1";
    aggregate_loop["run_id"] = options_.branch_run_id;
    aggregate_loop["workflow_id"] = future_workflow_id_;
    aggregate_loop["object_id"] = object_id_;
    if (!aggregate_loop.contains("iterations") || !aggregate_loop.at("iterations").is_array()) {
      aggregate_loop["iterations"] = json::array();
    }
    aggregate_loop["initial_seed"] = {
        {"seed_runtime_outputs_path", pathString(options_.seed_runtime_outputs)},
        {"trigger_frame_index", options_.trigger_frame_index},
        {"trigger_time_s", options_.trigger_time_s},
        {"trigger_event_id", options_.trigger_event_id},
    };

    aggregate_data["schema_version"] = "flightenv.platform.data_plane_manifest.v1";
    aggregate_data["run_id"] = options_.branch_run_id;
    aggregate_data["workflow_id"] = future_workflow_id_;
    aggregate_data["object_id"] = object_id_;
    if (!aggregate_data.contains("entries") || !aggregate_data.at("entries").is_array()) {
      aggregate_data["entries"] = json::array();
    }

    aggregate_checkpoint["schema_version"] = "flightenv.platform.state_checkpoint.v1";
    aggregate_checkpoint["run_id"] = options_.branch_run_id;
    aggregate_checkpoint["workflow_id"] = future_workflow_id_;
    aggregate_checkpoint["object_id"] = object_id_;
    if (!aggregate_checkpoint.contains("checkpoints") || !aggregate_checkpoint.at("checkpoints").is_array()) {
      aggregate_checkpoint["checkpoints"] = json::array();
    }

    aggregate_nodes["schema_version"] = "flightenv.platform.runtime_node_snapshot.v1";
    aggregate_nodes["run_id"] = options_.branch_run_id;
    aggregate_nodes["workflow_id"] = future_workflow_id_;
    aggregate_nodes["object_id"] = object_id_;
    if (!aggregate_nodes.contains("nodes") || !aggregate_nodes.at("nodes").is_array()) {
      aggregate_nodes["nodes"] = json::array();
    }

    auto write_worker_state = [&](const std::string& status,
                                  const std::string& stop_reason,
                                  const fs::path& latest_seed_path) {
      state = {
          {"schema_version", "flightenv.platform.branch_manager_state.v1"},
          {"branch_id", options_.branch_id},
          {"run_id", options_.branch_run_id},
          {"run_dir", pathString(options_.branch_run_dir)},
          {"status", status},
          {"stop_reason", stop_reason},
          {"target_iterations", target_iterations},
          {"chunk_iterations", chunk_iterations},
          {"completed_iterations", completed_iterations},
          {"next_chunk_index", chunk_index},
          {"latest_seed_runtime_outputs", pathString(latest_seed_path)},
          {"trigger_frame_index", options_.trigger_frame_index},
          {"trigger_time_s", options_.trigger_time_s},
          {"trigger_event_id", options_.trigger_event_id},
          {"control_ref", pathString(control_path)},
          {"created_at_utc", created_at},
          {"updated_at_utc", nowUtcIso()},
      };
      if (preserved_worker_args.is_array() && !preserved_worker_args.empty()) {
        state["worker_args"] = preserved_worker_args;
      }
      state["original_seed_runtime_outputs"] = pathString(options_.seed_runtime_outputs);
      writeJson(state_path, state);
    };

    auto write_root_artifacts = [&](const std::string& status,
                                    const std::string& stop_reason,
                                    const fs::path& latest_seed_path) {
      aggregate_loop["generated_at_utc"] = nowUtcIso();
      aggregate_loop["summary"] = {
          {"iteration_count", completed_iterations},
          {"failed_nodes", 0},
          {"stopped", status == "completed" || status == "stopped" || status == "failed"},
          {"stop_reason", stop_reason},
          {"target_iterations", target_iterations},
          {"chunk_iterations", chunk_iterations},
          {"base_dt_s", base_dt_s},
          {"output_period_s", output_period_s},
          {"held_output_count", countHeldOutputsInLoop(aggregate_loop)},
      };
      aggregate_data["generated_at_utc"] = nowUtcIso();
      aggregate_data["summary"] = {
          {"entry_count", arrayOrEmpty(aggregate_data.value("entries", json::array())).size()},
          {"field_artifact_count", countFieldArtifactEntries(aggregate_data)},
          {"chunk_count", chunk_index},
      };
      aggregate_checkpoint["generated_at_utc"] = nowUtcIso();
      aggregate_checkpoint["summary"] = {
          {"checkpoint_count", arrayOrEmpty(aggregate_checkpoint.value("checkpoints", json::array())).size()},
      };
      aggregate_nodes["generated_at_utc"] = nowUtcIso();
      aggregate_nodes["summary"] = {
          {"node_snapshot_count", arrayOrEmpty(aggregate_nodes.value("nodes", json::array())).size()},
      };
      writeJson(options_.branch_run_dir / "runtime_loop_summary.json", aggregate_loop);
      writeJson(options_.branch_run_dir / "data_plane_manifest.json", aggregate_data);
      writeJson(options_.branch_run_dir / "state_checkpoint.json", aggregate_checkpoint);
      writeJson(options_.branch_run_dir / "runtime_node_snapshot.json", aggregate_nodes);

      json latest_outputs = readJson(latest_seed_path);
      latest_outputs["run_id"] = options_.branch_run_id;
      latest_outputs["workflow_id"] = future_workflow_id_;
      latest_outputs["object_id"] = object_id_;
      latest_outputs["generated_at_utc"] = nowUtcIso();
      latest_outputs["last_loop_iteration_index"] = std::max(0, completed_iterations - 1);
      latest_outputs["branch_manager"] = {
          {"branch_id", options_.branch_id},
          {"status", status},
          {"completed_iterations", completed_iterations},
          {"target_iterations", target_iterations},
          {"latest_chunk_seed", pathString(latest_seed_path)},
      };
      writeJson(options_.branch_run_dir / "runtime_outputs.json", latest_outputs);

      const fs::path latest_chunk_dir = latest_seed_path.parent_path();
      const json latest_chunk_runtime_evidence =
          objectOrEmpty(readJsonIfExists(latest_chunk_dir / "runtime_evidence.json"));
      const json latest_chunk_summary =
          objectOrEmpty(latest_chunk_runtime_evidence.value("summary", json::object()));
      const fs::path latest_rate_transition_plan = latest_chunk_dir / "rate_transition_plan.json";
      if (fs::exists(latest_rate_transition_plan)) {
        fs::copy_file(
            latest_rate_transition_plan,
            options_.branch_run_dir / "rate_transition_plan.json",
            fs::copy_options::overwrite_existing);
      }

      writeJson(options_.branch_run_dir / "runtime_evidence.json",
                {
                    {"schema_version", "flightenv.platform.runtime_evidence.v1"},
                    {"run_id", options_.branch_run_id},
                    {"workflow_id", future_workflow_id_},
                    {"object_id", object_id_},
                    {"status", status},
                    {"generated_at_utc", nowUtcIso()},
                    {"execution_backend", options_.execution_backend},
                    {"runtime_zero_copy_mode", runtimeZeroCopyModeName(
                                                   resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode))},
                    {"typed_buffer_persistence", runtimeTypedBufferPersistenceName(
                                                     resolveRuntimeTypedBufferPersistence(
                                                         options_.typed_buffer_persistence))},
                    {"compiled_workflow_dir", pathString(options_.compiled_future_workflow)},
                    {"adapter_registry_ref", pathString(options_.adapter_registry)},
                    {"seed_runtime_outputs_ref", pathString(options_.seed_runtime_outputs)},
                    {"branch_id", options_.branch_id},
                    {"branch_manager_state_ref", pathString(state_path)},
                    {"summary",
                     {
                         {"iteration_count", completed_iterations},
                         {"target_iterations", target_iterations},
                         {"chunk_iterations", chunk_iterations},
                         {"base_dt_s", base_dt_s},
                         {"output_period_s", output_period_s},
                         {"field_artifact_count", countFieldArtifactEntries(aggregate_data)},
                         {"rate_transition_count", jsonInt(latest_chunk_summary, "rate_transition_count", 0)},
                         {"cross_rate_transition_count",
                          jsonInt(latest_chunk_summary, "cross_rate_transition_count", 0)},
                         {"runtime_rate_transition_count",
                          jsonInt(latest_chunk_summary, "runtime_rate_transition_count", 0)},
                         {"stop_reason", stop_reason},
                         {"trigger_frame_index", options_.trigger_frame_index},
                         {"trigger_time_s", options_.trigger_time_s},
                     }},
                    {"refs",
                     {
                         {"runtime_outputs", "runtime_outputs.json"},
                         {"runtime_loop_summary", "runtime_loop_summary.json"},
                         {"data_plane_manifest", "data_plane_manifest.json"},
                         {"state_checkpoint", "state_checkpoint.json"},
                         {"runtime_node_snapshot", "runtime_node_snapshot.json"},
                         {"rate_transition_plan", "rate_transition_plan.json"},
                     }},
                });
    };

    auto merge_branch_index_to_mainline = [&](const std::string& status,
                                              const std::string& stop_reason) {
      writeRuntimeBranchIndex(options_.branch_run_dir);
      const json branch_timeline = readJsonIfExists(options_.branch_run_dir / "run_timeline_index.json");
      const json branch_series = readJsonIfExists(options_.branch_run_dir / "series_manifest.json");
      ScopedDirectoryLock lock(options_.chain_dir / ".branch_index_merge.lock");

      json registry = readJsonIfExists(options_.chain_dir / "branch_registry.json");
      if (!registry.is_object()) {
        registry = json::object();
      }
      if (!registry.contains("branches") || !registry.at("branches").is_array()) {
        registry["branches"] = json::array();
      }

      json branch_record = {
          {"branch_id", options_.branch_id},
          {"branch_kind", "future_prediction"},
          {"parent_branch_id", kRealtimePredictionBranchId},
          {"workflow_id", future_workflow_id_},
          {"run_id", options_.branch_run_id},
          {"run_dir", pathString(options_.branch_run_dir)},
          {"status", status},
          {"priority", 10},
          {"updated_at_utc", nowUtcIso()},
          {"trigger_frame_index", options_.trigger_frame_index},
          {"trigger_time_s", options_.trigger_time_s},
          {"trigger_event_id", options_.trigger_event_id},
          {"seed_runtime_outputs_ref", pathString(options_.seed_runtime_outputs)},
          {"refs",
           {
               {"runtime_evidence", "runtime_evidence.json"},
               {"runtime_outputs", "runtime_outputs.json"},
               {"runtime_loop_summary", "runtime_loop_summary.json"},
               {"data_plane_manifest", "data_plane_manifest.json"},
               {"state_checkpoint", "state_checkpoint.json"},
               {"branch_manager_state", pathString(state_path)},
               {"branch_control", pathString(control_path)},
           }},
          {"summary",
           {
               {"manager", "background_branch_worker"},
               {"iteration_count", completed_iterations},
               {"target_iterations", target_iterations},
               {"chunk_iterations", chunk_iterations},
               {"base_dt_s", base_dt_s},
               {"output_period_s", output_period_s},
               {"field_artifact_count", countFieldArtifactEntries(aggregate_data)},
               {"stop_reason", stop_reason},
           }},
      };
      bool replaced = false;
      auto preserve_branch_presentation = [&](const json& existing) {
        for (const std::string key : {"display_name", "kind_label", "branch_roles",
                                      "is_realtime_prediction", "trigger_cause_event_id"}) {
          if (existing.contains(key)) {
            branch_record[key] = existing.at(key);
          }
        }
      };
      for (auto& branch : registry["branches"]) {
        if (jsonString(branch, "branch_id", "") == options_.branch_id) {
          const std::string created = jsonString(branch, "created_at_utc", "");
          preserve_branch_presentation(branch);
          branch = branch_record;
          if (!created.empty()) {
            branch["created_at_utc"] = created;
          }
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        branch_record["created_at_utc"] = nowUtcIso();
        registry["branches"].push_back(branch_record);
      }
      int completed_count = 0;
      int failed_count = 0;
      int running_count = 0;
      for (const auto& branch : registry["branches"]) {
        if (jsonString(branch, "branch_kind", "") != "future_prediction") {
          continue;
        }
        const std::string branch_status = jsonString(branch, "status", "");
        if (branch_status == "completed") {
          ++completed_count;
        } else if (branch_status == "failed") {
          ++failed_count;
        } else if (branch_status == "running" || branch_status == "queued" ||
                   branch_status == "stop_requested") {
          ++running_count;
        }
      }
      registry["schema_version"] = "flightenv.platform.branch_registry.v1";
      registry["run_id"] = options_.run_id_prefix;
      registry["workflow_id"] = online_workflow_id_;
      registry["object_id"] = object_id_;
      registry["generated_at_utc"] = nowUtcIso();
      registry["primary_branch_id"] = kOnlineFilterBranchId;
      registry["summary"] = {
          {"branch_count", registry["branches"].size()},
          {"online_branch_count", 1},
          {"realtime_prediction_branch_count", 1},
          {"completed_prediction_count", completed_count},
          {"failed_prediction_count", failed_count},
          {"running_prediction_count", running_count},
      };
      writeJson(options_.chain_dir / "branch_registry.json", registry);

      json timeline = readJsonIfExists(options_.chain_dir / "run_timeline_index.json");
      if (!timeline.is_object()) {
        timeline = json::object();
      }
      for (const std::string key : {"online_frames", "branch_steps", "artifact_refs",
                                    "qoi_refs", "checkpoint_refs", "prediction_runs"}) {
        if (!timeline.contains(key) || !timeline.at(key).is_array()) {
          timeline[key] = json::array();
        }
        json kept = json::array();
        for (const auto& item : timeline[key]) {
          if (!item.is_object() || jsonString(item, "branch_id", "") != options_.branch_id) {
            kept.push_back(item);
          }
        }
        timeline[key] = kept;
      }
      auto append_branch_items = [&](const char* key) {
        if (!branch_timeline.contains(key) || !branch_timeline.at(key).is_array()) {
          return;
        }
        for (auto item : branch_timeline.at(key)) {
          if (!item.is_object()) {
            continue;
          }
          const double branch_relative_time_s = publicTimeFromItem(item, 0.0);
          item["branch_relative_time_s"] = branch_relative_time_s;
          item["trigger_time_s"] = options_.trigger_time_s;
          item["mainline_time_origin_s"] = options_.trigger_time_s;
          offsetNestedTime(item, options_.trigger_time_s, options_.trigger_frame_index);
          item["branch_id"] = options_.branch_id;
          item["source_run_dir"] = pathString(options_.branch_run_dir);
          item["mainline_frame_index"] = options_.trigger_frame_index;
          timeline[key].push_back(item);
        }
      };
      append_branch_items("branch_steps");
      append_branch_items("artifact_refs");
      append_branch_items("qoi_refs");
      append_branch_items("checkpoint_refs");
      timeline["prediction_runs"].push_back({
          {"branch_id", options_.branch_id},
          {"run_id", options_.branch_run_id},
          {"run_dir", pathString(options_.branch_run_dir)},
          {"status", status},
          {"trigger_frame_index", options_.trigger_frame_index},
          {"trigger_time_s", options_.trigger_time_s},
          {"trigger_event_id", options_.trigger_event_id},
          {"seed_runtime_outputs", pathString(options_.seed_runtime_outputs)},
          {"iteration_count", completed_iterations},
          {"target_iterations", target_iterations},
          {"chunk_iterations", chunk_iterations},
          {"base_dt_s", base_dt_s},
          {"output_period_s", output_period_s},
          {"field_artifact_count", countFieldArtifactEntries(aggregate_data)},
          {"stop_reason", stop_reason},
      });

      timeline["schema_version"] = "flightenv.platform.run_timeline_index.v1";
      timeline["run_id"] = options_.run_id_prefix;
      timeline["workflow_id"] = online_workflow_id_;
      timeline["object_id"] = object_id_;
      timeline["generated_at_utc"] = nowUtcIso();
      timeline["source_root"] = pathString(options_.chain_dir);
      timeline["branch_registry_ref"] = "branch_registry.json";
      timeline["branches"] = registry["branches"];
      if (!timeline.contains("series_manifest_refs") || !timeline.at("series_manifest_refs").is_array()) {
        timeline["series_manifest_refs"] = json::array();
      }
      bool has_series_ref = false;
      for (const auto& ref : timeline["series_manifest_refs"]) {
        if (ref.is_object() && jsonString(ref, "branch_id", "") == options_.branch_id) {
          has_series_ref = true;
          break;
        }
      }
      if (!has_series_ref && branch_series.is_object()) {
        timeline["series_manifest_refs"].push_back({
            {"branch_id", options_.branch_id},
            {"ref", pathString(options_.branch_run_dir / "series_manifest.json")},
        });
      }
      timeline["summary"] = {
          {"branch_count", registry["branches"].size()},
          {"online_frame_count", timeline["online_frames"].size()},
          {"branch_step_count", timeline["branch_steps"].size()},
          {"artifact_ref_count", timeline["artifact_refs"].size()},
          {"qoi_ref_count", timeline["qoi_refs"].size()},
          {"prediction_run_count", timeline["prediction_runs"].size()},
      };
      writeJson(options_.chain_dir / "run_timeline_index.json", timeline);

      json summary = readJsonIfExists(options_.chain_dir / "mainline_summary.json");
      if (summary.is_object()) {
        summary["status"] = running_count > 0 ? "mainline_completed_branch_running" : "completed";
        if (!summary.contains("prediction") || !summary.at("prediction").is_object()) {
          summary["prediction"] = json::object();
        }
        const json summary_prediction = summary.at("prediction");
        summary["prediction"]["requested_branch_count"] =
            std::max(jsonInt(summary_prediction, "requested_branch_count", 0),
                     completed_count + failed_count + running_count);
        summary["prediction"]["run_count"] = completed_count + failed_count + running_count;
        summary["prediction"]["completed_branch_count"] = completed_count;
        summary["prediction"]["failed_branch_count"] = failed_count;
        summary["prediction"]["running_branch_count"] = running_count;
        summary["prediction"]["latest_branch_id"] = options_.branch_id;
        summary["prediction"]["latest_branch_status"] = status;
        summary["prediction"]["latest_branch_iterations"] = completed_iterations;
        summary["prediction"]["latest_branch_stop_reason"] = stop_reason;
        writeJson(options_.chain_dir / "mainline_summary.json", summary);
      }

      json progress = readJsonIfExists(options_.chain_dir / "mainline_progress.json");
      if (!progress.is_object()) {
        progress = json::object();
      }
      const int requested_count = std::max(completed_count + failed_count + running_count, 1);
      const double branch_percent =
          target_iterations <= 0 ? 0.0
                                 : std::min(100.0,
                                            100.0 * static_cast<double>(completed_iterations) /
                                                static_cast<double>(target_iterations));
      const double all_branch_percent =
          100.0 * static_cast<double>(completed_count + failed_count) /
          static_cast<double>(requested_count);
      const std::string progress_status =
          running_count > 0 ? "mainline_completed_branch_running"
                            : (failed_count > 0 ? "completed_with_branch_failures" : "completed");
      progress["schema_version"] = "flightenv.platform.mainline_progress.v1";
      progress["run_id_prefix"] = options_.run_id_prefix;
      progress["object_id"] = object_id_;
      progress["generated_at_utc"] = nowUtcIso();
      progress["status"] = progress_status;
      progress["stage"] = running_count > 0 ? "prediction_branch_live" : "completed";
      progress["message"] = running_count > 0
                                ? "在线主线已完成，后台预测分支正在按 chunk 增量生成多场时序数据"
                                : "在线主线与后台预测分支已完成";
      progress["total_progress_percent"] =
          running_count > 0 ? std::min(99.0, 65.0 + 35.0 * all_branch_percent / 100.0) : 100.0;
      progress["prediction"] = {
          {"workflow_id", future_workflow_id_},
          {"requested_runs", completed_count + failed_count + running_count},
          {"completed_runs", completed_count},
          {"failed_runs", failed_count},
          {"running_runs", running_count},
          {"progress_percent", all_branch_percent},
          {"latest_branch_id", options_.branch_id},
          {"latest_branch_status", status},
          {"latest_branch_iterations", completed_iterations},
          {"latest_branch_target_iterations", target_iterations},
          {"latest_branch_chunk_iterations", chunk_iterations},
          {"latest_branch_progress_percent", branch_percent},
          {"latest_branch_stop_reason", stop_reason},
      };
      writeJson(options_.chain_dir / "mainline_progress.json", progress);
    };

    std::string final_status = "running";
    std::string final_stop_reason = "";
    write_worker_state("running", "", latest_seed);

    std::unique_ptr<NativeWorkflowRunner> branch_runner;
    if (useNativeBackend()) {
      NativeWorkflowOptions native_options;
      native_options.workspace_root = options_.workspace_root;
      native_options.pdk_root = options_.pdk_root;
      native_options.compiled_workflow = options_.compiled_future_workflow;
      native_options.adapter_registry = options_.adapter_registry;
      native_options.require_adapter_registry = options_.require_adapter_registry;
      native_options.python = options_.python;
      native_options.runtime_zero_copy_mode = options_.runtime_zero_copy_mode;
      native_options.typed_buffer_persistence = options_.typed_buffer_persistence;
      branch_runner = std::make_unique<NativeWorkflowRunner>(std::move(native_options));
    }

    while (completed_iterations < target_iterations) {
      const json control = readJsonIfExists(control_path);
      const std::string command = jsonString(control, "command", "run");
      if (command == "stop") {
        final_status = "stopped";
        final_stop_reason = "operator_requested_stop";
        write_root_artifacts(final_status, final_stop_reason, latest_seed);
        write_worker_state(final_status, final_stop_reason, latest_seed);
        merge_branch_index_to_mainline(final_status, final_stop_reason);
        return 0;
      }

      const int remaining = target_iterations - completed_iterations;
      const int this_chunk_iterations = std::min(chunk_iterations, remaining);
      const int current_chunk_index = chunk_index;
      std::ostringstream chunk_name;
      chunk_name << "chunk_" << std::setw(6) << std::setfill('0') << current_chunk_index;
      const fs::path chunk_dir = chunks_root / chunk_name.str();
      const std::string chunk_run_id = options_.branch_run_id + "." + chunk_name.str();

      runCompiledWorkflow(
          options_.compiled_future_workflow,
          branch_runner.get(),
          chunk_dir,
          chunk_run_id,
          latest_seed,
          {},
          this_chunk_iterations,
          false,
          options_.branch_id,
          "future.prediction");
      writeRuntimeBranchIndex(chunk_dir);

      const json chunk_loop = objectOrEmpty(readJson(chunk_dir / "runtime_loop_summary.json"));
      const json chunk_data = objectOrEmpty(readJson(chunk_dir / "data_plane_manifest.json"));
      const json chunk_checkpoint = objectOrEmpty(readJsonIfExists(chunk_dir / "state_checkpoint.json"));
      const json chunk_nodes = objectOrEmpty(readJsonIfExists(chunk_dir / "runtime_node_snapshot.json"));
      const int produced_iterations =
          jsonInt(objectOrEmpty(chunk_loop.value("summary", json::object())),
                  "iteration_count",
                  this_chunk_iterations);
      if (produced_iterations <= 0) {
        throw std::runtime_error("Branch chunk produced zero iterations: " + pathString(chunk_dir));
      }

      const int previous_completed = completed_iterations;
      const double time_offset_s = base_dt_s > 0.0 ? base_dt_s * static_cast<double>(previous_completed) : 0.0;
      appendArrayWithMetadata(
          aggregate_loop["iterations"],
          arrayOrEmpty(chunk_loop.value("iterations", json::array())),
          previous_completed,
          current_chunk_index,
          chunk_dir,
          time_offset_s);
      appendArrayWithMetadata(
          aggregate_data["entries"],
          arrayOrEmpty(chunk_data.value("entries", json::array())),
          previous_completed,
          current_chunk_index,
          chunk_dir,
          time_offset_s);
      appendArrayWithMetadata(
          aggregate_checkpoint["checkpoints"],
          arrayOrEmpty(chunk_checkpoint.value("checkpoints", json::array())),
          previous_completed,
          current_chunk_index,
          chunk_dir,
          time_offset_s);
      appendArrayWithMetadata(
          aggregate_nodes["nodes"],
          arrayOrEmpty(chunk_nodes.value("nodes", json::array())),
          previous_completed,
          current_chunk_index,
          chunk_dir,
          time_offset_s);

      completed_iterations += produced_iterations;
      latest_seed = options_.branch_run_dir / "runtime_outputs.json";
      const fs::path chunk_seed = chunk_dir / "runtime_outputs.json";
      const bool terminal_stop =
          stopReasonMatchesTerminalPolicy(branchStopReasonFromLoop(chunk_loop), object_runtime_profile_);
      const bool limit_stop = completed_iterations >= target_iterations;
      final_stop_reason = terminal_stop ? branchStopReasonFromLoop(chunk_loop)
                                        : (limit_stop ? "target_iterations_reached" : "chunk_completed");
      final_status = terminal_stop || limit_stop ? "completed" : "running";
      chunk_index = current_chunk_index + 1;

      write_root_artifacts(final_status, final_stop_reason, chunk_seed);
      write_worker_state(final_status, final_stop_reason, latest_seed);
      merge_branch_index_to_mainline(final_status, final_stop_reason);

      if (terminal_stop || limit_stop) {
        return 0;
      }
    }
    return 0;
  } catch (const std::exception& exc) {
    std::cerr << "[ERROR] Branch worker failed: " << exc.what() << std::endl;
    try {
      fs::create_directories(options_.branch_run_dir);
      const fs::path state_path = branchManagerStatePath(options_.chain_dir, options_.branch_id);
      writeJson(state_path,
                {
                    {"schema_version", "flightenv.platform.branch_manager_state.v1"},
                    {"branch_id", options_.branch_id},
                    {"run_id", options_.branch_run_id},
                    {"run_dir", pathString(options_.branch_run_dir)},
                    {"status", "failed"},
                    {"reason", exc.what()},
                    {"updated_at_utc", nowUtcIso()},
                });
      writeJson(options_.branch_run_dir / "runtime_evidence.json",
                {
                    {"schema_version", "flightenv.platform.runtime_evidence.v1"},
                    {"run_id", options_.branch_run_id},
                    {"workflow_id", future_workflow_id_},
                    {"object_id", object_id_},
                    {"status", "failed"},
                    {"generated_at_utc", nowUtcIso()},
                    {"execution_backend", options_.execution_backend},
                    {"runtime_zero_copy_mode", runtimeZeroCopyModeName(
                                                   resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode))},
                    {"typed_buffer_persistence", runtimeTypedBufferPersistenceName(
                                                     resolveRuntimeTypedBufferPersistence(
                                                         options_.typed_buffer_persistence))},
                    {"branch_id", options_.branch_id},
                    {"summary", {{"reason", exc.what()}}},
                });
    } catch (...) {
    }
    return 2;
  }
}

int PlatformRuntimeHost::applyBranchControl() {
  try {
    if (options_.branch_id.empty()) {
      throw std::runtime_error("--branch-id is required for --branch-control");
    }
    fs::create_directories(options_.chain_dir / "branch_controls");
    const fs::path control_path = branchControlPath(options_.chain_dir, options_.branch_id);
    const fs::path state_path = branchManagerStatePath(options_.chain_dir, options_.branch_id);
    const std::string action = options_.branch_control_action;
    if (action != "stop" && action != "resume" && action != "status") {
      throw std::runtime_error("Unsupported branch control action: " + action);
    }
    if (action == "status") {
      const json state = readJsonIfExists(state_path);
      std::cout << state.dump(2, ' ', false, json::error_handler_t::replace) << std::endl;
      return 0;
    }
    writeJson(control_path,
              {
                  {"schema_version", "flightenv.platform.branch_control.v1"},
                  {"branch_id", options_.branch_id},
                  {"command", action == "stop" ? "stop" : "run"},
                  {"requested_at_utc", nowUtcIso()},
              });
    if (action == "stop") {
      json state = readJsonIfExists(state_path);
      state["schema_version"] = "flightenv.platform.branch_manager_state.v1";
      state["branch_id"] = options_.branch_id;
      state["status"] = "stop_requested";
      state["updated_at_utc"] = nowUtcIso();
      writeJson(state_path, state);
      std::cout << "[OK] Branch stop requested: " << options_.branch_id << std::endl;
      return 0;
    }

    json state = readJson(state_path);
    std::vector<std::string> args;
    if (state.contains("worker_args") && state.at("worker_args").is_array()) {
      for (const auto& item : state.at("worker_args")) {
        if (item.is_string()) {
          args.push_back(item.get<std::string>());
        }
      }
    }
    if (args.empty()) {
      const std::string branch_run_id = jsonString(state, "run_id", options_.run_id_prefix + "." + safeFileName(options_.branch_id));
      const std::string branch_run_dir = jsonString(state, "run_dir", "");
      const std::string latest_seed = jsonString(state, "latest_seed_runtime_outputs", "");
      const int target_iterations = jsonInt(state, "target_iterations", options_.future_max_iterations);
      const int chunk_iterations = jsonInt(state, "chunk_iterations", options_.branch_chunk_iterations);
      args = {
          pathString(options_.runtime_host_exe),
          "--branch-worker",
          "--resume-existing-branch",
          "--workspace-root", pathString(options_.workspace_root),
          "--pdk-root", pathString(options_.pdk_root),
          "--object-package-root", pathString(options_.object_package_root),
          "--compiled-online", pathString(options_.compiled_online_workflow),
          "--compiled-future", pathString(options_.compiled_future_workflow),
          "--adapter-registry", pathString(options_.adapter_registry),
          "--pdk-cli", pathString(options_.pdk_cli),
          "--pdk-run-script", pathString(options_.pdk_run_script),
          "--run-root", pathString(options_.run_root),
          "--chain-dir", pathString(options_.chain_dir),
          "--python", options_.python,
          "--execution-backend", options_.execution_backend,
          "--zero-copy-mode", options_.runtime_zero_copy_mode,
          "--typed-buffer-persistence", options_.typed_buffer_persistence,
          "--run-id-prefix", options_.run_id_prefix,
          "--future-max-iterations", std::to_string(target_iterations),
          "--branch-chunk-iterations", std::to_string(chunk_iterations),
          "--branch-id", options_.branch_id,
          "--branch-run-id", branch_run_id,
      };
      if (!branch_run_dir.empty()) {
        args.push_back("--branch-run-dir");
        args.push_back(branch_run_dir);
      }
      if (!latest_seed.empty()) {
        args.push_back("--seed-runtime-outputs");
        args.push_back(latest_seed);
      }
    } else {
      args.push_back("--resume-existing-branch");
    }
    unsigned long pid = 0;
    const fs::path log_path = options_.chain_dir / "branch_worker_logs" /
                              (safeFileName(options_.branch_id) + ".resume.log");
    if (!launchDetachedProcess(args, options_.workspace_root, log_path, &pid)) {
      throw std::runtime_error("failed to launch branch worker for resume");
    }
    state["status"] = "running";
    state["resume_process_id"] = pid;
    state["worker_log_ref"] = pathString(log_path);
    state["updated_at_utc"] = nowUtcIso();
    writeJson(state_path, state);
    std::cout << "[OK] Branch resumed: " << options_.branch_id << " pid=" << pid << std::endl;
    return 0;
  } catch (const std::exception& exc) {
    std::cerr << "[ERROR] Branch control failed: " << exc.what() << std::endl;
    return 2;
  }
}

void PlatformRuntimeHost::executeOnlineLoop() {
  std::cout << "[C++ RuntimeHost] 初始化分支注册表..." << std::endl;
  initializeBranchRegistry();
  std::cout << "[C++ RuntimeHost] 读取外部观测流..." << std::endl;
  const json external_frames = loadExternalFrames();
  const int requested_frames = options_.online_frames > 0
                                   ? options_.online_frames
                                   : static_cast<int>(external_frames.size());
  const int target_frames = std::min<int>(requested_frames, static_cast<int>(external_frames.size()));
  if (target_frames <= 0) {
    throw std::runtime_error("No external frames available for online loop.");
  }
  std::cout << "[C++ RuntimeHost] 外部观测帧数=" << target_frames << std::endl;

  fs::path previous_seed;
  double previous_time = 0.0;
  bool has_previous_time = false;
  for (int i = 0; i < target_frames; ++i) {
    const json frame = external_frames.at(static_cast<std::size_t>(i));
    const json frame_payload =
        frame.is_object() && frame.contains("frame") && frame.at("frame").is_object()
            ? frame.at("frame")
            : frame;
    const double sample_time = jsonDouble(frame_payload, "sample_time_s",
                                          jsonDouble(frame_payload, "time_s", i));
    current_tick_index_ = i;
    current_source_time_s_ = sample_time;
    current_delta_t_s_ = has_previous_time ? std::max(0.0, sample_time - previous_time) : 0.0;
    current_run_time_s_ = i == 0 ? 0.0 : current_run_time_s_ + current_delta_t_s_;
    has_previous_time = true;
    previous_time = sample_time;

    if (options_.replay_by_platform_clock && current_delta_t_s_ > 0.0 && options_.replay_time_scale > 0.0) {
      const auto sleep_ms = static_cast<int>(1000.0 * current_delta_t_s_ / options_.replay_time_scale);
      if (sleep_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
      }
    }

    current_stage_ = "online_running";
    current_status_ = "running";
    current_message_ = "外部观测帧到达，推进在线滤波: frame=" + std::to_string(i);
    std::cout << "[C++ RuntimeHost] 写入在线帧进度 frame=" << i << std::endl;
    {
      StateLock lock(*this);
      writeProgressLocked();
    }
    std::cout << "[C++ RuntimeHost] 调用在线帧 workflow frame=" << i << std::endl;

    const fs::path online_run_dir = runOneOnlineFrame(frame, i, previous_seed);
    const fs::path runtime_outputs = online_run_dir / "runtime_outputs.json";
    std::string posterior_event_id;
    {
      StateLock lock(*this);
      completed_online_frames_ = i + 1;
      appendRuntimeIndex(online_run_dir, kOnlineFilterBranchId, true, i, sample_time);
      for (auto& branch : branch_records_) {
        const std::string branch_id = jsonString(branch, "branch_id", "");
        if (branch_id == kOnlineFilterBranchId) {
          branch["updated_at_utc"] = nowUtcIso();
          branch["summary"] = {
              {"frame_count", completed_online_frames_},
              {"step_count", countItemsForBranch(branch_steps_, kOnlineFilterBranchId)},
              {"artifact_count", countItemsForBranch(artifact_refs_, kOnlineFilterBranchId)},
              {"qoi_ref_count", countItemsForBranch(qoi_refs_, kOnlineFilterBranchId)},
              {"checkpoint_count", countItemsForBranch(checkpoint_refs_, kOnlineFilterBranchId)},
              {"latest_runtime_outputs", pathString(runtime_outputs)},
          };
        } else if (branch_id == kRealtimePredictionBranchId) {
          branch["updated_at_utc"] = nowUtcIso();
          branch["summary"] = {
              {"frame_count", completed_online_frames_},
              {"step_count", countItemsForBranch(branch_steps_, kRealtimePredictionBranchId)},
              {"artifact_count", countItemsForBranch(artifact_refs_, kRealtimePredictionBranchId)},
              {"qoi_ref_count", countItemsForBranch(qoi_refs_, kRealtimePredictionBranchId)},
              {"checkpoint_count", countItemsForBranch(checkpoint_refs_, kRealtimePredictionBranchId)},
              {"latest_runtime_outputs", pathString(runtime_outputs)},
          };
        }
      }
      posterior_event_id = appendRuntimeEventLocked(
          "posterior_frame_committed",
          kOnlineFilterBranchId,
          i,
          sample_time,
          {
              {"workflow_id", online_workflow_id_},
              {"run_dir", pathString(online_run_dir)},
              {"runtime_outputs_ref", pathString(runtime_outputs)},
              {"seed_from_previous_frame_ref", previous_seed.empty() ? "" : pathString(previous_seed)},
              {"source_observation_frame", frame},
          });
      writeRuntimeIndexesLocked("live_tail");
      writeProgressLocked();
    }
    maybeForkPredictionBranch(i, frame, runtime_outputs, posterior_event_id);
    previous_seed = runtime_outputs;
  }
}

void PlatformRuntimeHost::waitForPredictionBranches() {
  current_stage_ = "prediction_wait";
  current_message_ = "在线主线已完成，等待后台预测分支收口";
  {
      StateLock lock(*this);
    writeProgressLocked();
  }
  for (auto& thread : branch_threads_) {
    std::cout << "[C++ RuntimeHost] 等待预测分支 thread..." << std::endl;
    if (thread.joinable()) {
      thread.join();
    }
  }
  branch_threads_.clear();

  if (!options_.branch_manager_enabled) {
    return;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(30);
  while (std::chrono::steady_clock::now() < deadline) {
    json registry = readJsonIfExists(options_.chain_dir / "branch_registry.json");
    json branches = json::array();
    if (registry.is_object() && registry.contains("branches") && registry.at("branches").is_array()) {
      branches = registry.at("branches");
    }
    if (!branches.is_array() || branches.empty()) {
      branches = branch_records_;
    }

    bool has_pending_branch = false;
    bool has_live_worker = false;
    int completed_count = 0;
    int failed_count = 0;
    int running_count = 0;

    for (const auto& branch : branches) {
      if (!branch.is_object() || jsonString(branch, "branch_kind", "") != "future_prediction") {
        continue;
      }
      const std::string branch_id = jsonString(branch, "branch_id", "");
      if (branch_id.empty()) {
        continue;
      }

      const fs::path state_path = branchManagerStatePath(options_.chain_dir, branch_id);
      json state = readJsonIfExists(state_path);
      const json branch_summary =
          branch.contains("summary") && branch.at("summary").is_object() ? branch.at("summary") : json::object();
      const std::string status = jsonString(state, "status", jsonString(branch, "status", ""));
      const int target_iterations =
          std::max(1, jsonInt(state, "target_iterations",
                              jsonInt(branch_summary, "target_iterations", options_.future_max_iterations)));
      const int completed_iterations =
          jsonInt(state, "completed_iterations", jsonInt(branch_summary, "iteration_count", 0));

      if (status == "completed" || completed_iterations >= target_iterations) {
        ++completed_count;
        continue;
      }
      if (status == "failed") {
        ++failed_count;
        continue;
      }
      if (status == "stopped") {
        ++completed_count;
        continue;
      }

      has_pending_branch = true;
      ++running_count;
      const unsigned long process_id = static_cast<unsigned long>(
          std::max(jsonInt(branch_summary, "process_id", 0), jsonInt(state, "resume_process_id", 0)));
      if (processStillRunning(process_id)) {
        has_live_worker = true;
        continue;
      }

      std::cout << "[C++ RuntimeHost] 续跑预测分支至收口: " << branch_id
                << " completed=" << completed_iterations << "/" << target_iterations << std::endl;
      HostOptions branch_options = options_;
      branch_options.branch_worker = true;
      branch_options.resume_existing_branch = true;
      branch_options.branch_control_action.clear();
      branch_options.prepare_only = false;
      branch_options.branch_id = branch_id;
      branch_options.branch_run_id = jsonString(state, "run_id", jsonString(branch, "run_id", ""));
      branch_options.branch_run_dir = jsonString(state, "run_dir", jsonString(branch, "run_dir", ""));
      branch_options.seed_runtime_outputs =
          jsonString(state, "original_seed_runtime_outputs",
                     jsonString(branch, "seed_runtime_outputs_ref",
                                jsonString(state, "latest_seed_runtime_outputs", "")));
      branch_options.trigger_frame_index =
          jsonInt(state, "trigger_frame_index", jsonInt(branch, "trigger_frame_index", -1));
      branch_options.trigger_time_s =
          jsonDouble(state, "trigger_time_s", jsonDouble(branch, "trigger_time_s", 0.0));
      branch_options.trigger_event_id =
          jsonString(state, "trigger_event_id", jsonString(branch, "trigger_event_id", ""));

      PlatformRuntimeHost branch_host(std::move(branch_options));
      const int rc = branch_host.runBranchWorker();
      if (rc != 0) {
        ++failed_prediction_runs_;
      }
    }

    completed_prediction_runs_.store(completed_count);
    failed_prediction_runs_.store(failed_count);
    requested_prediction_runs_ =
        std::max(requested_prediction_runs_, completed_count + failed_count + running_count);

    if (!has_pending_branch) {
      break;
    }
    if (has_live_worker) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }

  if (std::chrono::steady_clock::now() >= deadline) {
    throw std::runtime_error("Timed out waiting for prediction branches to finish.");
  }
}

nlohmann::json PlatformRuntimeHost::buildHealthLedgerLocked() const {
  auto compact_checkpoint = [](const json& checkpoint) {
    if (!checkpoint.is_object()) {
      return json::object();
    }
    json compact = {
        {"checkpoint_id", jsonString(checkpoint, "checkpoint_id")},
        {"node_id", jsonString(checkpoint, "node_id")},
        {"operator_id", jsonString(checkpoint, "operator_id")},
        {"branch_id", jsonString(checkpoint, "branch_id")},
        {"source_run_dir", jsonString(checkpoint, "source_run_dir")},
        {"mainline_frame_index", jsonInt(checkpoint, "mainline_frame_index", -1)},
        {"loop_iteration_index", jsonInt(checkpoint, "loop_iteration_index", -1)},
        {"checkpoint_kind", jsonString(checkpoint, "checkpoint_kind")},
        {"replay_mode", jsonString(checkpoint, "replay_mode")},
        {"adapter_protocol", jsonString(checkpoint, "adapter_protocol")},
        {"time_point", checkpoint.value("time_point", json::object())},
        {"input_hashes", checkpoint.value("input_hashes", json::array())},
        {"output_hashes", checkpoint.value("output_hashes", json::array())},
    };
    return compact;
  };

  auto checkpoint_refs_for = [this, &compact_checkpoint](const std::string& branch_id, int frame_index) {
    json refs = json::array();
    for (const auto& checkpoint : checkpoint_refs_) {
      if (!checkpoint.is_object()) {
        continue;
      }
      if (jsonString(checkpoint, "branch_id") != branch_id) {
        continue;
      }
      const int checkpoint_frame = jsonInt(checkpoint, "mainline_frame_index", frame_index);
      if (frame_index >= 0 && checkpoint_frame != frame_index) {
        continue;
      }
      refs.push_back(compact_checkpoint(checkpoint));
    }
    return refs;
  };

  json entries = json::array();
  json latest_online_state = json::object();
  for (const auto& frame : online_frames_) {
    if (!frame.is_object()) {
      continue;
    }
    const int frame_index = jsonInt(frame, "frame_index", jsonInt(frame, "mainline_frame_index", -1));
    const double time_s = jsonDouble(frame, "sample_time_s", jsonDouble(frame, "time_s", 0.0));
    json entry = {
        {"entry_kind", "online_posterior_state"},
        {"branch_id", kOnlineFilterBranchId},
        {"frame_index", frame_index},
        {"time_s", time_s},
        {"runtime_outputs_ref", jsonString(frame, "source_runtime_outputs")},
        {"source_run_dir", jsonString(frame, "source_run_dir")},
        {"checkpoint_refs", checkpoint_refs_for(kOnlineFilterBranchId, frame_index)},
        {"semantics",
         {
             {"is_fact_state", true},
             {"may_seed_next_online_frame", true},
             {"may_seed_future_prediction_branch", true},
         }},
    };
    entries.push_back(entry);
    latest_online_state = entry;
  }

  json prediction_entries = json::array();
  json latest_prediction_health = json::object();
  for (const auto& prediction : prediction_runs_) {
    if (!prediction.is_object()) {
      continue;
    }
    const fs::path run_dir = jsonString(prediction, "run_dir");
    const json trend = run_dir.empty() ? json::object() : readJsonIfExists(run_dir / "health_trend_summary.json");
    const bool has_health_trend = !run_dir.empty() && fs::exists(run_dir / "health_trend_summary.json");
    const json prediction_health_metrics =
        collectConfiguredMetrics(prediction, trend, object_runtime_profile_, "prediction_health_metrics");
    json entry = {
        {"entry_kind", "future_prediction_health"},
        {"branch_id", jsonString(prediction, "branch_id")},
        {"trigger_frame_index", jsonInt(prediction, "trigger_frame_index", -1)},
        {"trigger_time_s", jsonDouble(prediction, "trigger_time_s", 0.0)},
        {"trigger_event_id", jsonString(prediction, "trigger_event_id")},
        {"seed_runtime_outputs_ref", jsonString(prediction, "seed_runtime_outputs")},
        {"run_id", jsonString(prediction, "run_id")},
        {"run_dir", jsonString(prediction, "run_dir")},
        {"status", jsonString(prediction, "status")},
        {"iteration_count", jsonInt(prediction, "iteration_count", 0)},
        {"stop_reason", jsonString(prediction, "stop_reason")},
        {"field_artifact_count", jsonInt(prediction, "field_artifact_count", 0)},
        {"health_trend_ref", has_health_trend ? pathString(run_dir / "health_trend_summary.json") : ""},
        {"semantics",
         {
             {"is_fact_state", false},
             {"is_prediction_branch_result", true},
             {"does_not_mutate_online_mainline", true},
         }},
    };
    for (auto it = prediction_health_metrics.begin(); it != prediction_health_metrics.end(); ++it) {
      entry[it.key()] = it.value();
    }
    prediction_entries.push_back(entry);
    entries.push_back(entry);
    latest_prediction_health = entry;
  }

  json accumulated_state_refs = json::array();
  for (const auto& checkpoint : checkpoint_refs_) {
    if (checkpoint.is_object() && matchesAccumulatedStatePolicy(checkpoint, object_runtime_profile_)) {
      accumulated_state_refs.push_back(compact_checkpoint(checkpoint));
    }
  }

  const std::string continuity_note =
      jsonString(objectOrEmpty(object_runtime_profile_).value("health_ledger", json::object()),
                 "state_continuity_note",
                 "Prediction QoI results do not overwrite the online fact state. "
                 "Only object-declared accumulated state checkpoints can be used as restart baseline when explicitly selected.");

  const json state_continuity = {
      {"current_fact_state_ref", latest_online_state.value("runtime_outputs_ref", "")},
      {"latest_online_checkpoint_refs", latest_online_state.value("checkpoint_refs", json::array())},
      {"accumulated_state_checkpoint_refs", accumulated_state_refs},
      {"prediction_seed_policy", "future branches inherit trigger-frame posterior runtime_outputs/checkpoints"},
      {"next_run_seed_policy", "prefer latest executable checkpoint; otherwise latest online runtime_outputs"},
      {"notes", continuity_note},
  };

  return {
      {"schema_version", "flightenv.platform.health_ledger.v1"},
      {"run_id", options_.run_id_prefix},
      {"object_id", object_id_},
      {"generated_at_utc", nowUtcIso()},
      {"ledger_role", "state_continuity_and_health_prediction_index"},
      {"entries", entries},
      {"online_state_entries",
       {
           {"count", online_frames_.size()},
           {"latest", latest_online_state},
       }},
      {"prediction_health_entries",
       {
           {"count", prediction_entries.size()},
           {"latest", latest_prediction_health},
           {"entries", prediction_entries},
       }},
      {"state_continuity", state_continuity},
      {"summary",
       {
           {"online_frame_count", online_frames_.size()},
           {"prediction_run_count", prediction_runs_.size()},
           {"checkpoint_ref_count", checkpoint_refs_.size()},
           {"accumulated_state_checkpoint_count", accumulated_state_refs.size()},
           {"qoi_ref_count", qoi_refs_.size()},
       }},
  };
}

void PlatformRuntimeHost::writeHealthLedgerLocked() const {
  const json ledger = buildHealthLedgerLocked();
  writeJson(options_.chain_dir / "health_ledger.json", ledger);
  writeJson(options_.chain_dir / "health_ledger_summary.json",
            {
                {"schema_version", "flightenv.platform.health_ledger_summary.v1"},
                {"run_id", options_.run_id_prefix},
                {"object_id", object_id_},
                {"generated_at_utc", nowUtcIso()},
                {"health_ledger_ref", "health_ledger.json"},
                {"state_continuity", objectOrEmpty(ledger.value("state_continuity", json::object()))},
                {"summary", objectOrEmpty(ledger.value("summary", json::object()))},
                {"latest_prediction_health",
                 objectOrEmpty(ledger.value("prediction_health_entries", json::object()))
                     .value("latest", json::object())},
            });
}

void PlatformRuntimeHost::writeRuntimeIndexesLocked(const std::string& cursor_mode) {
  const std::string generated = nowUtcIso();
  const json existing_registry = readJsonIfExists(options_.chain_dir / "branch_registry.json");
  const json existing_timeline = readJsonIfExists(options_.chain_dir / "run_timeline_index.json");

  auto is_main_branch = [](const std::string& branch_id) {
    return branch_id == kOnlineFilterBranchId || branch_id == kRealtimePredictionBranchId;
  };
  auto upsert_by_branch_id = [](json& target, const json& record) {
    if (!record.is_object()) {
      return;
    }
    const std::string branch_id = jsonString(record, "branch_id", "");
    if (branch_id.empty()) {
      return;
    }
    for (auto& item : target) {
      if (item.is_object() && jsonString(item, "branch_id", "") == branch_id) {
        item = record;
        return;
      }
    }
    target.push_back(record);
  };
  auto merge_branch_scoped_array = [&](const char* key, const json& memory_items) {
    json merged = json::array();
    if (existing_timeline.is_object() && existing_timeline.contains(key) &&
        existing_timeline.at(key).is_array()) {
      for (const auto& item : existing_timeline.at(key)) {
        if (!item.is_object()) {
          continue;
        }
        if (!is_main_branch(jsonString(item, "branch_id", ""))) {
          merged.push_back(item);
        }
      }
    }
    for (const auto& item : memory_items) {
      merged.push_back(item);
    }
    return merged;
  };

  json merged_branch_records = branch_records_;
  if (existing_registry.is_object() && existing_registry.contains("branches") &&
      existing_registry.at("branches").is_array()) {
    for (const auto& branch : existing_registry.at("branches")) {
      const std::string branch_id = jsonString(branch, "branch_id", "");
      if (!branch_id.empty() && !is_main_branch(branch_id)) {
        upsert_by_branch_id(merged_branch_records, branch);
      }
    }
  }
  branch_records_ = merged_branch_records;

  json merged_prediction_runs = prediction_runs_;
  if (existing_timeline.is_object() && existing_timeline.contains("prediction_runs") &&
      existing_timeline.at("prediction_runs").is_array()) {
    for (const auto& prediction : existing_timeline.at("prediction_runs")) {
      upsert_by_branch_id(merged_prediction_runs, prediction);
    }
  }
  prediction_runs_ = merged_prediction_runs;

  // Branch workers update their own manager state and timeline entry after the
  // mainline process has already registered a branch as queued/running. Before
  // writing the platform-visible registry, reconcile those durable records so
  // the UI does not keep showing a stale queued branch after the worker exits.
  for (auto& branch : branch_records_) {
    if (!branch.is_object() || jsonString(branch, "branch_kind", "") != "future_prediction") {
      continue;
    }
    const std::string branch_id = jsonString(branch, "branch_id", "");
    if (branch_id.empty()) {
      continue;
    }

    json state = readJsonIfExists(branchManagerStatePath(options_.chain_dir, branch_id));
    bool state_has_terminal_status = false;
    if (state.is_object()) {
      const std::string state_status = jsonString(state, "status", "");
      if (!state_status.empty()) {
        branch["status"] = state_status;
        state_has_terminal_status =
            state_status == "completed" || state_status == "failed" || state_status == "stopped";
      }
      branch["updated_at_utc"] = jsonString(state, "updated_at_utc", nowUtcIso());
      json summary =
          branch.contains("summary") && branch.at("summary").is_object() ? branch.at("summary") : json::object();
      summary["completed_iterations"] = jsonInt(state, "completed_iterations", 0);
      summary["iteration_count"] = jsonInt(state, "completed_iterations", jsonInt(summary, "iteration_count", 0));
      summary["target_iterations"] = jsonInt(state, "target_iterations", jsonInt(summary, "target_iterations", 0));
      summary["chunk_iterations"] = jsonInt(state, "chunk_iterations", jsonInt(summary, "chunk_iterations", 0));
      const std::string stop_reason = jsonString(state, "stop_reason", "");
      if (!stop_reason.empty()) {
        summary["stop_reason"] = stop_reason;
      }
      branch["summary"] = summary;
    }

    for (const auto& prediction : prediction_runs_) {
      if (!prediction.is_object() || jsonString(prediction, "branch_id", "") != branch_id) {
        continue;
      }
      const std::string prediction_status = jsonString(prediction, "status", "");
      if (!prediction_status.empty() && !state_has_terminal_status) {
        branch["status"] = prediction_status;
      }
      json summary =
          branch.contains("summary") && branch.at("summary").is_object() ? branch.at("summary") : json::object();
      for (const std::string key : {"base_dt_s", "chunk_iterations", "field_artifact_count",
                                    "iteration_count", "output_period_s", "stop_reason",
                                    "target_iterations"}) {
        if (prediction.contains(key)) {
          const bool state_owns_branch_progress =
              state_has_terminal_status &&
              (key == "chunk_iterations" || key == "iteration_count" ||
               key == "stop_reason" || key == "target_iterations");
          if (state_owns_branch_progress && summary.contains(key)) {
            continue;
          }
          summary[key] = prediction.at(key);
        }
      }
      branch["summary"] = summary;
      break;
    }
  }

  json merged_branch_steps = merge_branch_scoped_array("branch_steps", branch_steps_);
  json merged_artifact_refs = merge_branch_scoped_array("artifact_refs", artifact_refs_);
  json merged_qoi_refs = merge_branch_scoped_array("qoi_refs", qoi_refs_);
  json merged_checkpoint_refs = merge_branch_scoped_array("checkpoint_refs", checkpoint_refs_);

  auto remove_branch_items = [](json& target, const std::string& branch_id) {
    if (!target.is_array() || branch_id.empty()) {
      return;
    }
    json kept = json::array();
    for (const auto& item : target) {
      if (!item.is_object() || jsonString(item, "branch_id", "") != branch_id) {
        kept.push_back(item);
      }
    }
    target = kept;
  };
  auto append_branch_timeline_items = [&](json& target,
                                          const json& branch_timeline,
                                          const char* key,
                                          const json& branch,
                                          const fs::path& branch_run_dir) {
    if (!branch_timeline.is_object() || !branch_timeline.contains(key) ||
        !branch_timeline.at(key).is_array()) {
      return;
    }
    const std::string branch_id = jsonString(branch, "branch_id", "");
    const double trigger_time = jsonDouble(branch, "trigger_time_s", 0.0);
    const int trigger_frame = jsonInt(branch, "trigger_frame_index", -1);
    remove_branch_items(target, branch_id);
    for (auto item : branch_timeline.at(key)) {
      if (!item.is_object()) {
        continue;
      }
      const double branch_relative_time_s = publicTimeFromItem(item, 0.0);
      item["branch_relative_time_s"] = branch_relative_time_s;
      item["trigger_time_s"] = trigger_time;
      item["mainline_time_origin_s"] = trigger_time;
      offsetNestedTime(item, trigger_time, trigger_frame);
      item["branch_id"] = branch_id;
      item["source_run_dir"] = pathString(branch_run_dir);
      item["mainline_frame_index"] = trigger_frame;
      target.push_back(item);
    }
  };
  for (const auto& branch : branch_records_) {
    if (!branch.is_object() || jsonString(branch, "branch_kind", "") != "future_prediction") {
      continue;
    }
    const std::string branch_id = jsonString(branch, "branch_id", "");
    const std::string branch_run_dir_text = jsonString(branch, "run_dir", "");
    if (branch_id.empty() || branch_run_dir_text.empty()) {
      continue;
    }
    const fs::path branch_run_dir(branch_run_dir_text);
    const json branch_timeline = readJsonIfExists(branch_run_dir / "run_timeline_index.json");
    append_branch_timeline_items(merged_branch_steps, branch_timeline, "branch_steps", branch, branch_run_dir);
    append_branch_timeline_items(merged_artifact_refs, branch_timeline, "artifact_refs", branch, branch_run_dir);
    append_branch_timeline_items(merged_qoi_refs, branch_timeline, "qoi_refs", branch, branch_run_dir);
    append_branch_timeline_items(merged_checkpoint_refs, branch_timeline, "checkpoint_refs", branch, branch_run_dir);

    int display_field_artifact_count = 0;
    const json branch_artifact_refs = arrayOrEmpty(branch_timeline.value("artifact_refs", json::array()));
    for (const auto& artifact_ref : branch_artifact_refs) {
      if (artifact_ref.is_object() && jsonString(artifact_ref, "representation", "") == "artifact_ref") {
        ++display_field_artifact_count;
      }
    }

    const json summary =
        branch.contains("summary") && branch.at("summary").is_object() ? branch.at("summary") : json::object();
    json prediction = {
        {"branch_id", branch_id},
        {"run_id", jsonString(branch, "run_id", branch_run_dir.filename().string())},
        {"run_dir", branch_run_dir_text},
        {"status", jsonString(branch, "status", "")},
        {"trigger_frame_index", jsonInt(branch, "trigger_frame_index", -1)},
        {"trigger_time_s", jsonDouble(branch, "trigger_time_s", 0.0)},
        {"trigger_event_id", jsonString(branch, "trigger_event_id", "")},
        {"seed_runtime_outputs", jsonString(branch, "seed_runtime_outputs_ref", "")},
        {"iteration_count", jsonInt(summary, "iteration_count", jsonInt(summary, "completed_iterations", 0))},
        {"target_iterations", jsonInt(summary, "target_iterations", 0)},
        {"chunk_iterations", jsonInt(summary, "chunk_iterations", 0)},
        {"field_artifact_count",
         display_field_artifact_count > 0 ? display_field_artifact_count
                                          : jsonInt(summary, "field_artifact_count", 0)},
        {"field_artifact_ref_count", display_field_artifact_count},
        {"stop_reason", jsonString(summary, "stop_reason", "")},
    };
    if (summary.contains("base_dt_s")) {
      prediction["base_dt_s"] = summary.at("base_dt_s");
    }
    if (summary.contains("output_period_s")) {
      prediction["output_period_s"] = summary.at("output_period_s");
    }
    upsert_by_branch_id(prediction_runs_, prediction);
  }

  int prediction_count = 0;
  int completed_prediction_count = 0;
  int failed_prediction_count = 0;
  int running_prediction_count = 0;
  for (const auto& branch : branch_records_) {
    if (!branch.is_object() || jsonString(branch, "branch_kind", "") != "future_prediction") {
      continue;
    }
    ++prediction_count;
    const std::string status = jsonString(branch, "status", "");
    if (status == "completed" || status == "stopped") {
      ++completed_prediction_count;
    } else if (status == "failed") {
      ++failed_prediction_count;
    } else if (status == "running" || status == "queued" || status == "stop_requested") {
      ++running_prediction_count;
    }
  }
  completed_prediction_runs_.store(completed_prediction_count);
  failed_prediction_runs_.store(failed_prediction_count);
  requested_prediction_runs_ = std::max(requested_prediction_runs_, prediction_count);

  const json registry = {
      {"schema_version", "flightenv.platform.branch_registry.v1"},
      {"run_id", options_.run_id_prefix},
      {"workflow_id", online_workflow_id_},
      {"object_id", object_id_},
      {"generated_at_utc", generated},
      {"primary_branch_id", kOnlineFilterBranchId},
      {"branches", branch_records_},
      {"summary",
       {
           {"branch_count", branch_records_.size()},
           {"online_branch_count", 1},
           {"realtime_prediction_branch_count", 1},
           {"prediction_branch_count", std::max(0, prediction_count)},
           {"completed_prediction_count", completed_prediction_count},
           {"failed_prediction_count", failed_prediction_count},
           {"running_prediction_count", running_prediction_count},
       }},
  };
  writeJson(options_.chain_dir / "branch_registry.json", registry);

  json cursor = {
      {"schema_version", "flightenv.platform.runtime_cursor.v1"},
      {"cursor_id", "ui.active"},
      {"run_id", options_.run_id_prefix},
      {"object_id", object_id_},
      {"generated_at_utc", generated},
      {"mode", cursor_mode},
      {"branch_id", kOnlineFilterBranchId},
      {"follow_live", cursor_mode == "live_tail"},
      {"status", current_status_},
      {"refs",
       {
           {"run_timeline_index", "run_timeline_index.json"},
           {"runtime_events", "runtime_events.json"},
           {"health_ledger", "health_ledger.json"},
       }},
      {"frame_index", std::max(0, completed_online_frames_ - 1)},
      {"time_s", current_source_time_s_},
  };
  writeJson(options_.chain_dir / "runtime_cursor.json", cursor);

  json series = json::array();
  for (auto it = series_by_id_.begin(); it != series_by_id_.end(); ++it) {
    series.push_back(it.value());
  }
  const json series_manifest = {
      {"schema_version", "flightenv.platform.series_manifest.v1"},
      {"run_id", options_.run_id_prefix},
      {"workflow_id", online_workflow_id_},
      {"object_id", object_id_},
      {"branch_id", "*"},
      {"generated_at_utc", generated},
      {"default_x_axis", "time_s"},
      {"series", series},
      {"summary",
       {
           {"series_count", series.size()},
           {"point_count", std::accumulate(series.begin(), series.end(), 0,
                                           [](int acc, const json& item) {
                                             return acc + static_cast<int>(item.value("points", json::array()).size());
                                           })},
           {"branch_count", branch_records_.size()},
       }},
  };
  writeJson(options_.chain_dir / "series_manifest.json", series_manifest);

  json series_manifest_refs = json::array({{{"branch_id", "*"}, {"ref", "series_manifest.json"}}});
  if (existing_timeline.is_object() && existing_timeline.contains("series_manifest_refs") &&
      existing_timeline.at("series_manifest_refs").is_array()) {
    std::set<std::string> seen_refs;
    for (const auto& ref : series_manifest_refs) {
      seen_refs.insert(jsonString(ref, "branch_id", "") + "|" + jsonString(ref, "ref", ""));
    }
    for (const auto& ref : existing_timeline.at("series_manifest_refs")) {
      if (!ref.is_object()) {
        continue;
      }
      const std::string key = jsonString(ref, "branch_id", "") + "|" + jsonString(ref, "ref", "");
      if (seen_refs.insert(key).second) {
        series_manifest_refs.push_back(ref);
      }
    }
  }

  const json timeline = {
      {"schema_version", "flightenv.platform.run_timeline_index.v1"},
      {"run_id", options_.run_id_prefix},
      {"workflow_id", online_workflow_id_},
      {"object_id", object_id_},
      {"generated_at_utc", generated},
      {"source_root", pathString(options_.chain_dir)},
      {"branch_registry_ref", "branch_registry.json"},
      {"cursor_ref", "runtime_cursor.json"},
      {"runtime_events_ref", "runtime_events.json"},
      {"health_ledger_ref", "health_ledger.json"},
      {"branches", branch_records_},
      {"runtime_events", runtime_events_},
      {"online_frames", online_frames_},
      {"branch_steps", merged_branch_steps},
      {"artifact_refs", merged_artifact_refs},
      {"qoi_refs", merged_qoi_refs},
      {"checkpoint_refs", merged_checkpoint_refs},
      {"series_manifest_refs", series_manifest_refs},
      {"prediction_runs", prediction_runs_},
      {"summary",
       {
           {"branch_count", branch_records_.size()},
           {"online_frame_count", online_frames_.size()},
           {"branch_step_count", merged_branch_steps.size()},
           {"artifact_ref_count", merged_artifact_refs.size()},
           {"qoi_ref_count", merged_qoi_refs.size()},
           {"prediction_run_count", prediction_runs_.size()},
           {"series_count", series.size()},
           {"runtime_event_count", runtime_events_.size()},
           {"runtime_event_loop", runtime_event_loop_.summary()},
       }},
  };
  writeJson(options_.chain_dir / "run_timeline_index.json", timeline);

  const json runtime_events = {
      {"schema_version", "flightenv.platform.runtime_events.v1"},
      {"run_id", options_.run_id_prefix},
      {"workflow_id", online_workflow_id_},
      {"object_id", object_id_},
      {"generated_at_utc", generated},
      {"events", runtime_events_},
      {"summary", {{"event_count", runtime_events_.size()},
                   {"runtime_event_loop", runtime_event_loop_.summary()}}},
  };
  writeJson(options_.chain_dir / "runtime_events.json", runtime_events);

  json sensor_stream = {
      {"schema_version", "flightenv.platform.sensor_stream.v1"},
      {"run_id", options_.run_id_prefix + ".online"},
      {"workflow_id", online_workflow_id_},
      {"object_id", object_id_},
      {"generated_at_utc", generated},
      {"source_operator_id", "external.measurement_driver"},
      {"source_stream_path", pathString(options_.external_observation_stream)},
      {"frames", online_frames_},
      {"summary",
       {
           {"frame_count", online_frames_.size()},
           {"first_sample_time_s",
            online_frames_.empty() ? json(nullptr) : json(online_frames_.front().value("sample_time_s", 0.0))},
           {"last_sample_time_s",
            online_frames_.empty() ? json(nullptr) : json(online_frames_.back().value("sample_time_s", 0.0))},
           {"input_exhausted", false},
       }},
  };
  writeJson(options_.chain_dir / "sensor_stream.json", sensor_stream);
  writeHealthLedgerLocked();
  writeRuntimeHostEvidenceLocked(current_status_);
}

void PlatformRuntimeHost::writeProgressLocked() {
  const int online_requested = std::max(1, options_.online_frames);
  const double online_progress = 100.0 * static_cast<double>(completed_online_frames_) /
                                 static_cast<double>(online_requested);
  const int requested_predictions = std::max(1, requested_prediction_runs_);
  const double prediction_progress =
      requested_prediction_runs_ <= 0
          ? 0.0
          : 100.0 * static_cast<double>(completed_prediction_runs_.load() + failed_prediction_runs_.load()) /
                static_cast<double>(requested_predictions);
  const double total_progress =
      std::min(100.0, 10.0 + 55.0 * online_progress / 100.0 + 35.0 * prediction_progress / 100.0);
  const json progress = {
      {"schema_version", "flightenv.platform.mainline_progress.v1"},
      {"run_id_prefix", options_.run_id_prefix},
      {"object_id", object_id_},
      {"generated_at_utc", nowUtcIso()},
      {"status", current_status_},
      {"stage", current_stage_},
      {"message", current_message_},
      {"execution_backend", options_.execution_backend},
      {"runtime_zero_copy_mode", runtimeZeroCopyModeName(
                                     resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode))},
      {"typed_buffer_persistence", runtimeTypedBufferPersistenceName(
                                      resolveRuntimeTypedBufferPersistence(
                                          options_.typed_buffer_persistence))},
      {"total_progress_percent", total_progress},
      {"clock",
       {
           {"source", "wall_or_external_stream"},
           {"run_time_s", current_run_time_s_},
           {"source_time_s", current_source_time_s_},
           {"tick_index", current_tick_index_},
           {"delta_t_s", current_delta_t_s_},
           {"playback_mode", options_.replay_by_platform_clock ? "platform_clock_wall_replay" : "event_driven_fast"},
       }},
      {"initialization",
       {
           {"status", fs::exists(options_.chain_dir / "operator_initialization.json") ? "prepared" : "pending"},
           {"snapshot_path", pathString(options_.chain_dir / "operator_initialization.json")},
       }},
      {"online",
       {
           {"workflow_id", online_workflow_id_},
           {"phase", current_stage_ == "online_running" ? "event_driven_filtering" : current_stage_},
           {"requested_frames", options_.online_frames},
           {"completed_frames", completed_online_frames_},
           {"progress_percent", online_progress},
           {"input_driver", "external_observation_stream"},
           {"input_stream", pathString(options_.external_observation_stream)},
       }},
      {"prediction",
       {
           {"workflow_id", future_workflow_id_},
           {"prediction_every_frames", effective_prediction_every_frames_},
           {"future_max_iterations", options_.future_max_iterations},
           {"requested_runs", requested_prediction_runs_},
           {"completed_runs", completed_prediction_runs_.load()},
           {"failed_runs", failed_prediction_runs_.load()},
           {"progress_percent", prediction_progress},
           {"runs", prediction_runs_},
       }},
  };
  writeJson(options_.chain_dir / "mainline_progress.json", progress);
}

void PlatformRuntimeHost::writeRuntimeHostEvidenceLocked(const std::string& status) {
  const json evidence = {
      {"schema_version", "flightenv.platform.cpp_runtime_host_evidence.v1"},
      {"run_id", options_.run_id_prefix},
      {"object_id", object_id_},
      {"status", status},
      {"generated_at_utc", nowUtcIso()},
      {"host",
       {
           {"implementation", "FlightEnvPlatformRuntimeHost.cpp"},
           {"role", "C++ online mainline and prediction branch scheduler"},
           {"execution_backend", options_.execution_backend},
           {"runtime_zero_copy_mode", runtimeZeroCopyModeName(
                                          resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode))},
           {"typed_buffer_persistence", runtimeTypedBufferPersistenceName(
                                           resolveRuntimeTypedBufferPersistence(
                                               options_.typed_buffer_persistence))},
           {"in_process_adapter_sessions", useNativeBackend()},
           {"branch_execution_mode",
            options_.branch_manager_enabled ? "background_branch_worker_process"
                                            : "native_in_process_branch_threads"},
           {"legacy_process_backend_allowed", options_.allow_legacy_process_backend},
           {"runtime_run_indexer", "cpp_host_internal"},
           {"wildcard_adapter_policy", "disabled_for_native_production"},
           {"native_session_note",
            useNativeBackend()
                ? "C++ Host directly manages adapter session lifecycle; DLL adapters stay in-process and PDK workflow process is not spawned."
                : "Compatibility mode: C++ Host keeps mainline/branch scheduling while operator execution uses the PDK compiled-workflow runner."},
           {"note",
            useNativeBackend()
                ? "调度、分支、adapter 生命周期均由 C++ Host 接管；PDK workflow 进程不再参与算子执行热路径。"
                : "调度、分支由 C++ Host 接管；算子执行后端显式使用 PDK compiled-workflow runner 兼容模式。"},
       }},
      {"inputs",
       {
           {"compiled_online_workflow", pathString(options_.compiled_online_workflow)},
           {"compiled_future_workflow", pathString(options_.compiled_future_workflow)},
           {"adapter_registry", pathString(options_.adapter_registry)},
           {"external_observation_stream", pathString(options_.external_observation_stream)},
           {"object_runtime_profile", jsonString(object_runtime_profile_, "_profile_path")},
       }},
      {"outputs",
       {
           {"branch_registry", "branch_registry.json"},
           {"runtime_cursor", "runtime_cursor.json"},
           {"run_timeline_index", "run_timeline_index.json"},
           {"runtime_events", "runtime_events.json"},
           {"series_manifest", "series_manifest.json"},
           {"health_ledger", "health_ledger.json"},
           {"health_ledger_summary", "health_ledger_summary.json"},
           {"mainline_progress", "mainline_progress.json"},
           {"mainline_summary", "mainline_summary.json"},
       }},
      {"summary",
       {
           {"online_frame_count", completed_online_frames_},
           {"runtime_zero_copy_mode", runtimeZeroCopyModeName(
                                          resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode))},
           {"typed_buffer_persistence", runtimeTypedBufferPersistenceName(
                                           resolveRuntimeTypedBufferPersistence(
                                               options_.typed_buffer_persistence))},
           {"requested_prediction_count", requested_prediction_runs_},
           {"completed_prediction_count", completed_prediction_runs_.load()},
           {"failed_prediction_count", failed_prediction_runs_.load()},
           {"max_concurrent_branches", effective_max_concurrent_branches_},
           {"runtime_event_count", runtime_events_.size()},
           {"online_native_session_summary",
            useNativeBackend() && online_native_runner_ ? online_native_runner_->sessionSummary() : json::object()},
       }},
  };
  writeJson(options_.chain_dir / "runtime_host_evidence.json", evidence);
}

void PlatformRuntimeHost::writeFinalSummary(const std::string& status) {
  StateLock lock(*this);
  const bool mainline_completed =
      status == "completed" || status == "completed_with_branch_failures" ||
      status == "mainline_completed_branch_running";
  current_status_ = status;
  if (status == "mainline_completed_branch_running") {
    current_stage_ = "prediction_running";
    current_message_ = "在线主线已完成，预测分支仍在后台运行";
  } else if (status == "completed_with_branch_failures") {
    current_stage_ = "completed_with_branch_failures";
    current_message_ = "在线主线已完成，部分预测分支失败";
  } else {
    current_stage_ = status == "completed" ? "completed" : "failed";
    current_message_ = status == "completed" ? "C++ Runtime Host 主线与预测分支完成"
                                              : "C++ Runtime Host 运行失败";
  }
  for (auto& branch : branch_records_) {
    const std::string branch_id = jsonString(branch, "branch_id", "");
    if (branch_id == kOnlineFilterBranchId || branch_id == kRealtimePredictionBranchId) {
      branch["status"] = mainline_completed ? "completed" : "failed";
      branch["updated_at_utc"] = nowUtcIso();
    }
  }

  auto count_event_kind = [this](const std::string& kind) {
    int count = 0;
    for (const auto& event : runtime_events_) {
      if (jsonString(event, "event_kind", "") == kind) {
        ++count;
      }
    }
    return count;
  };
  const int posterior_event_count = count_event_kind("posterior_frame_committed");
  const int branch_triggered_event_count = count_event_kind("branch_triggered");
  const int queued_event_count = count_event_kind("prediction_branch_queued");
  const int finished_event_count = count_event_kind("prediction_branch_completed") +
                                   count_event_kind("prediction_branch_failed");
  json summary_prediction_runs = prediction_runs_;
  const json existing_timeline = readJsonIfExists(options_.chain_dir / "run_timeline_index.json");
  if (existing_timeline.is_object() && existing_timeline.contains("prediction_runs") &&
      existing_timeline.at("prediction_runs").is_array()) {
    for (const auto& prediction : existing_timeline.at("prediction_runs")) {
      if (!prediction.is_object()) {
        continue;
      }
      const std::string branch_id = jsonString(prediction, "branch_id", "");
      if (branch_id.empty()) {
        continue;
      }
      bool replaced = false;
      for (auto& item : summary_prediction_runs) {
        if (item.is_object() && jsonString(item, "branch_id", "") == branch_id) {
          item = prediction;
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        summary_prediction_runs.push_back(prediction);
      }
    }
  }
  int summary_completed_prediction_runs = 0;
  int summary_failed_prediction_runs = 0;
  for (const auto& prediction : summary_prediction_runs) {
    if (!prediction.is_object()) {
      continue;
    }
    const std::string prediction_status = jsonString(prediction, "status", "");
    if (prediction_status == "completed" || prediction_status == "stopped") {
      ++summary_completed_prediction_runs;
    } else if (prediction_status == "failed") {
      ++summary_failed_prediction_runs;
    }
  }
  const int summary_finished_prediction_runs =
      summary_completed_prediction_runs + summary_failed_prediction_runs;

  completed_prediction_runs_.store(summary_completed_prediction_runs);
  failed_prediction_runs_.store(summary_failed_prediction_runs);
  requested_prediction_runs_ =
      std::max(requested_prediction_runs_, static_cast<int>(summary_prediction_runs.size()));

  const json summary = {
      {"schema_version", "flightenv.platform.cpp_runtime_host_mainline.v1"},
      {"run_id_prefix", options_.run_id_prefix},
      {"object_id", object_id_},
      {"generated_at_utc", nowUtcIso()},
      {"status", status},
      {"host",
       {
           {"execution_backend", options_.execution_backend},
           {"in_process_adapter_sessions", useNativeBackend()},
           {"runtime_zero_copy_mode", runtimeZeroCopyModeName(
                                          resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode))},
           {"typed_buffer_persistence", runtimeTypedBufferPersistenceName(
                                           resolveRuntimeTypedBufferPersistence(
                                               options_.typed_buffer_persistence))},
       }},
      {"online",
       {
           {"workflow_id", online_workflow_id_},
           {"requested_frames", options_.online_frames},
           {"effective_frames", completed_online_frames_},
           {"sensor_stream_path", pathString(options_.chain_dir / "sensor_stream.json")},
           {"input_driver", "external_observation_stream"},
           {"external_observation_stream", pathString(options_.external_observation_stream)},
       }},
      {"prediction",
       {
           {"workflow_id", future_workflow_id_},
           {"prediction_every_frames", effective_prediction_every_frames_},
           {"future_max_iterations", options_.future_max_iterations},
            {"run_count", summary_prediction_runs.size()},
            {"runs", summary_prediction_runs},
        }},
      {"runtime_events",
       {
           {"event_count", runtime_events_.size()},
           {"posterior_frame_committed_count", posterior_event_count},
           {"branch_triggered_count", branch_triggered_event_count},
           {"prediction_branch_queued_count", queued_event_count},
           {"prediction_branch_finished_count", std::max(finished_event_count, summary_finished_prediction_runs)},
           {"runtime_events_ref", "runtime_events.json"},
       }},
      {"health_ledger",
       {
           {"health_ledger_ref", "health_ledger.json"},
           {"health_ledger_summary_ref", "health_ledger_summary.json"},
       }},
      {"acceptance",
       {
           {"cpp_runtime_host_b2_ok", true},
           {"external_measurement_port_b3_ok", completed_online_frames_ > 0},
           {"prediction_branch_b4_ok", requested_prediction_runs_ > 0},
           {"posterior_event_branch_handoff_ok",
            posterior_event_count >= completed_online_frames_ &&
                branch_triggered_event_count >= requested_prediction_runs_ &&
                queued_event_count >= requested_prediction_runs_ &&
                std::max(finished_event_count, summary_finished_prediction_runs) >=
                    completed_prediction_runs_.load() + failed_prediction_runs_.load()},
           {"mainline_not_aborted_by_branch_failure", true},
           {"all_completed_branches_have_indexes", true},
       }},
  };
  writeJson(options_.chain_dir / "mainline_summary.json", summary);
  writeRuntimeIndexesLocked("mainline_replay_frame");
  writeProgressLocked();
  writeRuntimeHostEvidenceLocked(status);
}

int PlatformRuntimeHost::run() {
  try {
    resolveDefaults();
    if (!options_.branch_control_action.empty()) {
      return applyBranchControl();
    }
    if (options_.branch_worker) {
      return runBranchWorker();
    }
    ensureInputs();
    loadCompiledWorkflowMetadata();
    std::cout << "[C++ RuntimeHost] workspace = " << pathString(options_.workspace_root) << std::endl;
    std::cout << "[C++ RuntimeHost] chain_dir = " << pathString(options_.chain_dir) << std::endl;
    prepareRuntime();
    if (options_.prepare_only) {
      current_status_ = "completed";
      current_stage_ = "completed";
      current_message_ = "初始化完成，等待外部观测驱动";
      std::cout << "[C++ RuntimeHost] prepare-only 收口写入..." << std::endl;
      writeProgressLocked();
      writeRuntimeHostEvidenceLocked("prepared");
      std::cout << "[C++ RuntimeHost] prepare-only 完成" << std::endl;
      return 0;
    }
    executeOnlineLoop();
    const bool has_running_branches =
        requested_prediction_runs_ > completed_prediction_runs_.load() + failed_prediction_runs_.load();
    if (has_running_branches) {
      writeFinalSummary("mainline_completed_branch_running");
    }
    if (has_running_branches && options_.branch_manager_enabled &&
        !options_.wait_for_prediction_branches) {
      std::cout << "[OK] C++ RuntimeHost online mainline completed; prediction branches continue in background."
                << std::endl;
      std::cout << "  evidence = " << pathString(options_.chain_dir) << std::endl;
      return 0;
    }
    waitForPredictionBranches();
    {
      StateLock lock(*this);
      writeRuntimeIndexesLocked("completed");
    }
    {
      const json registry = readJsonIfExists(options_.chain_dir / "branch_registry.json");
      if (registry.is_object() && registry.contains("branches") && registry.at("branches").is_array()) {
        int completed_count = 0;
        int failed_count = 0;
        int running_count = 0;
        for (const auto& branch : registry.at("branches")) {
          if (!branch.is_object() || jsonString(branch, "branch_kind", "") != "future_prediction") {
            continue;
          }
          const std::string branch_status = jsonString(branch, "status", "");
          if (branch_status == "completed" || branch_status == "stopped") {
            ++completed_count;
          } else if (branch_status == "failed") {
            ++failed_count;
          } else if (branch_status == "running" || branch_status == "queued" ||
                     branch_status == "stop_requested") {
            ++running_count;
          }
        }
        completed_prediction_runs_.store(completed_count);
        failed_prediction_runs_.store(failed_count);
        requested_prediction_runs_ =
            std::max(requested_prediction_runs_, completed_count + failed_count + running_count);
      }
    }
    writeFinalSummary(failed_prediction_runs_.load() > 0 ? "completed_with_branch_failures" : "completed");
    std::cout << "[OK] C++ RuntimeHost online mainline completed." << std::endl;
    std::cout << "  evidence = " << pathString(options_.chain_dir) << std::endl;
    return 0;
  } catch (const std::exception& exc) {
    std::cerr << "[ERROR] C++ RuntimeHost failed: " << exc.what() << std::endl;
    try {
      writeFinalSummary("failed");
    } catch (...) {
    }
    return 2;
  }
}

int RunPlatformRuntimeHostCli(int argc, char** argv) {
  try {
    const auto args = parseOptions(argc, argv);
    if (args.count("help") || args.count("h")) {
      printUsage();
      return 0;
    }

    HostOptions options;
    if (args.count("workspace-root")) options.workspace_root = args.at("workspace-root");
    if (args.count("pdk-root")) options.pdk_root = args.at("pdk-root");
    if (args.count("object-package-root")) options.object_package_root = args.at("object-package-root");
    if (args.count("compiled-online")) options.compiled_online_workflow = args.at("compiled-online");
    if (args.count("compiled-future")) options.compiled_future_workflow = args.at("compiled-future");
    if (args.count("adapter-registry")) options.adapter_registry = args.at("adapter-registry");
    if (args.count("external-observation-stream")) {
      options.external_observation_stream = args.at("external-observation-stream");
    }
    if (args.count("pdk-run-script")) options.pdk_run_script = args.at("pdk-run-script");
    if (args.count("pdk-cli")) options.pdk_cli = args.at("pdk-cli");
    if (args.count("run-root")) options.run_root = args.at("run-root");
    if (args.count("chain-dir")) options.chain_dir = args.at("chain-dir");
    if (args.count("python")) options.python = args.at("python");
    if (args.count("execution-backend")) options.execution_backend = args.at("execution-backend");
    if (args.count("zero-copy-mode")) options.runtime_zero_copy_mode = args.at("zero-copy-mode");
    if (args.count("typed-buffer-persistence")) {
      options.typed_buffer_persistence = args.at("typed-buffer-persistence");
    }
    if (args.count("run-id-prefix")) options.run_id_prefix = args.at("run-id-prefix");
    if (args.count("online-frames")) options.online_frames = std::stoi(args.at("online-frames"));
    if (args.count("prediction-every-frames")) {
      options.prediction_every_frames = std::stoi(args.at("prediction-every-frames"));
    }
    if (args.count("future-max-iterations")) {
      options.future_max_iterations = std::stoi(args.at("future-max-iterations"));
    }
    if (args.count("max-concurrent-branches")) {
      options.max_concurrent_branches = std::stoi(args.at("max-concurrent-branches"));
    }
    if (args.count("branch-chunk-iterations")) {
      options.branch_chunk_iterations = std::stoi(args.at("branch-chunk-iterations"));
    }
    if (args.count("branch-id")) options.branch_id = args.at("branch-id");
    if (args.count("branch-run-id")) options.branch_run_id = args.at("branch-run-id");
    if (args.count("branch-run-dir")) options.branch_run_dir = args.at("branch-run-dir");
    if (args.count("seed-runtime-outputs")) options.seed_runtime_outputs = args.at("seed-runtime-outputs");
    if (args.count("runtime-host-exe")) options.runtime_host_exe = args.at("runtime-host-exe");
    if (args.count("trigger-event-id")) options.trigger_event_id = args.at("trigger-event-id");
    if (args.count("trigger-frame-index")) {
      options.trigger_frame_index = std::stoi(args.at("trigger-frame-index"));
    }
    if (args.count("trigger-time-s")) {
      options.trigger_time_s = std::stod(args.at("trigger-time-s"));
    }
    if (args.count("branch-control")) {
      options.branch_control_action = args.at("branch-control");
    }
    options.prepare_only = args.count("prepare-only") > 0;
    options.preflight_adapters = args.count("preflight-adapters") > 0;
    options.branch_worker = args.count("branch-worker") > 0;
    options.resume_existing_branch = args.count("resume-existing-branch") > 0;
    options.wait_for_prediction_branches = args.count("wait-for-branches") > 0;
    options.branch_manager_enabled = args.count("no-branch-manager") == 0;
    options.require_adapter_registry = args.count("require-adapter-registry") > 0 ||
                                       args.count("no-require-adapter-registry") == 0;
    options.allow_legacy_process_backend = args.count("allow-legacy-process-backend") > 0;
    options.replay_by_platform_clock = args.count("replay-by-platform-clock") > 0;
    if (args.count("replay-time-scale")) {
      options.replay_time_scale = std::stod(args.at("replay-time-scale"));
    }
    if (options.runtime_host_exe.empty() && argc > 0 && argv[0] != nullptr) {
      options.runtime_host_exe = argv[0];
    }
    PlatformRuntimeHost host(std::move(options));
    return host.run();
  } catch (const std::exception& exc) {
    std::cerr << "[ERROR] " << exc.what() << std::endl;
    printUsage();
    return 2;
  }
}

}  // namespace FlightEnvPlatformRuntime
