/**
 * @file NativeWorkflowRunner.cpp
 * @brief 实现本地 workflow 执行器。
 *
 * 大概：这是 platform-runtime 当前最核心的执行路径实现。
 * 具体：它负责解析对象包和 workflow、准备端口、调用 adapter/operator、推进时间和收集输出。
 * 被谁使用：被 PlatformRuntimeHost、runtime CLI、端到端测试和 launcher/SDK 间接使用。
 * 使用谁：使用 PDK 契约、AdapterSession、time 组件、nlohmann::json、文件系统和数据面引用。
 * 拆分判断：能总结但明显偏大；后续应继续拆 backend executor、port binding、materialization、evidence writer。
 */

#include "FlightEnvPlatformRuntime/NativeWorkflowRunner.hpp"

#include "FlightEnvPlatform/Adapter/AdapterAbi.hpp"
#include "FlightEnvPlatform/Runtime/ReadyQueueScheduler.hpp"
#include "FlightEnvPlatform/Runtime/RuntimePacket.hpp"
#include "FlightEnvPlatform/Runtime/ThreadPoolExecutor.hpp"
#include "FlightEnvPlatform/Runtime/ThreadSafePortStore.hpp"
#include "FlightEnvPlatformRuntime/AdapterSession.hpp"
#include "FlightEnvPlatformRuntime/RuntimeAdapterInvoker.hpp"
#include "FlightEnvPlatformRuntime/RuntimeEventLoop.hpp"
#include "FlightEnvPlatformRuntime/RuntimeEvidenceWriter.hpp"
#include "FlightEnvPlatformRuntime/RuntimeNodeEvidenceBuilder.hpp"
#include "FlightEnvPlatformRuntime/RuntimePortBindingResolver.hpp"
#include "FlightEnvPlatformRuntime/RuntimePortPacketWriter.hpp"
#include "FlightEnvPlatformRuntime/RuntimeReadyQueueExecutor.hpp"
#include "FlightEnvPlatformRuntime/RuntimePublicFrameBuilder.hpp"
#include "FlightEnvPlatformRuntime/RuntimeTimeScheduler.hpp"
#include "FlightEnvPlatformRuntime/RuntimeTypedBufferStore.hpp"
#include "FlightEnvPlatformRuntime/RuntimeZeroCopyPolicy.hpp"
#include "FlightEnvPlatformRuntime/estimation/RuntimeEstimationService.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeEventQueue.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeInputAlignment.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeMaterialization.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimePortSampleBuffer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

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

std::int64_t steadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string pathString(const fs::path& path) {
  return path.lexically_normal().string();
}

std::string adapterRunDirString(const fs::path& path) {
#ifdef _WIN32
  const fs::path absolute = fs::absolute(path).lexically_normal();
  const std::string normal = absolute.string();
  if (normal.rfind(R"(\\?\)", 0) == 0) {
    return normal;
  }
  const std::wstring wide = absolute.wstring();
  if (wide.rfind(LR"(\\)", 0) == 0) {
    return fs::path(LR"(\\?\UNC\)" + wide.substr(2)).string();
  }
  return fs::path(LR"(\\?\)" + wide).string();
#else
  return pathString(path);
#endif
}

json readJson(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("JSON file not found: " + pathString(path));
  }
  json value;
  input >> value;
  return value;
}

json readJsonIfExists(const fs::path& path) {
  if (!fs::exists(path)) {
    return json::object();
  }
  return readJson(path);
}

void writeJson(const fs::path& path, const json& value) {
  if (!path.parent_path().empty()) {
    fs::create_directories(path.parent_path());
  }
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("Cannot write JSON file: " + pathString(path));
  }
  output << value.dump(2) << '\n';
}

void appendTrace(const fs::path& run_dir, const std::string& message) {
  if (run_dir.empty()) {
    return;
  }
  try {
    fs::create_directories(run_dir);
    std::ofstream output(run_dir / "native_runner_trace.log", std::ios::app | std::ios::binary);
    if (output) {
      output << nowUtcIso() << " " << message << '\n';
    }
  } catch (...) {
  }
}

std::string jsonString(const json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

double jsonDouble(const json& value, const std::string& key, double fallback = 0.0) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
    return fallback;
  }
  return value.at(key).get<double>();
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

std::uint64_t jsonUInt64(const json& value, const std::string& key, std::uint64_t fallback = 0) {
  if (!value.is_object() || !value.contains(key)) {
    return fallback;
  }
  const auto& item = value.at(key);
  if (item.is_number_unsigned()) {
    return item.get<std::uint64_t>();
  }
  if (item.is_number_integer()) {
    const auto signed_value = item.get<std::int64_t>();
    return signed_value > 0 ? static_cast<std::uint64_t>(signed_value) : fallback;
  }
  if (item.is_number()) {
    const auto double_value = item.get<double>();
    return double_value > 0.0 ? static_cast<std::uint64_t>(double_value) : fallback;
  }
  return fallback;
}

bool jsonBool(const json& value, const std::string& key, bool fallback = false) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean()) {
    return fallback;
  }
  return value.at(key).get<bool>();
}

bool envFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return false;
  }
  std::string text(value);
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text == "1" || text == "true" || text == "on" || text == "yes";
}

std::uint64_t fnv64(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string digestJson(const json& value) {
  const std::string text = value.dump(-1, ' ', false, json::error_handler_t::replace);
  std::ostringstream out;
  out << "fnv64:" << std::hex << std::setw(16) << std::setfill('0') << fnv64(text);
  return out.str();
}

json summarizeJson(const json& value) {
  const std::string text = value.dump(-1, ' ', false, json::error_handler_t::replace);
  json summary = {
      {"type", value.type_name()},
      {"byte_size", static_cast<int>(text.size())},
      {"digest_algorithm", "fnv64"},
      {"digest", digestJson(value)},
  };
  if (value.is_object()) {
    json keys = json::array();
    int count = 0;
    for (auto it = value.begin(); it != value.end() && count < 32; ++it, ++count) {
      keys.push_back(it.key());
    }
    summary["keys"] = keys;
  }
  if (value.is_array()) {
    summary["array_size"] = value.size();
  }
  return summary;
}

std::string stableId(std::string text) {
  std::replace_if(text.begin(), text.end(), [](unsigned char c) {
    return !(std::isalnum(c) || c == '_' || c == '-' || c == '.');
  }, '_');
  return text;
}

std::string runtimeExecutionTag(const json& time_info, int iteration_index, const std::string& node_id) {
  const json runtime_event = time_info.value("runtime_event", json::object());
  const std::string event_id = jsonString(runtime_event, "event_id");
  if (!event_id.empty()) {
    return stableId(event_id);
  }
  return "step_" + std::to_string(iteration_index) + "." + stableId(node_id);
}

std::string hashId(const std::string& prefix, const std::string& text) {
  std::ostringstream out;
  out << prefix << "." << std::hex << std::setw(16) << std::setfill('0') << fnv64(text);
  return out.str();
}

std::string replaceAll(std::string value, const std::string& from, const std::string& to) {
  std::size_t pos = 0;
  while ((pos = value.find(from, pos)) != std::string::npos) {
    value.replace(pos, from.size(), to);
    pos += to.size();
  }
  return value;
}

std::string resolveTemplatePath(std::string value, const NativeWorkflowOptions& options) {
  value = replaceAll(value, "{workspace_root}", pathString(options.workspace_root));
  value = replaceAll(value, "{pdk_root}", pathString(options.pdk_root));
  value = replaceAll(value, "{python}", options.python.empty() ? std::string("python") : options.python);
  return value;
}

std::string quoteProcessArg(const std::string& text) {
  if (text.empty()) {
    return "\"\"";
  }
  bool needs_quotes = false;
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) || c == '"') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    return text;
  }
  std::string out;
  out.push_back('"');
  int backslashes = 0;
  for (const char c : text) {
    if (c == '\\') {
      ++backslashes;
      continue;
    }
    if (c == '"') {
      out.append(static_cast<std::size_t>(backslashes * 2 + 1), '\\');
      out.push_back('"');
      backslashes = 0;
      continue;
    }
    if (backslashes > 0) {
      out.append(static_cast<std::size_t>(backslashes), '\\');
      backslashes = 0;
    }
    out.push_back(c);
  }
  if (backslashes > 0) {
    out.append(static_cast<std::size_t>(backslashes * 2), '\\');
  }
  out.push_back('"');
  return out;
}

std::string resolveAdapterCommandTemplate(
    std::string value,
    const NativeWorkflowOptions& options,
    const json& context,
    const fs::path& request_json,
    const fs::path& response_json) {
  value = resolveTemplatePath(std::move(value), options);
  value = replaceAll(value, "{request_json}", pathString(request_json));
  value = replaceAll(value, "{response_json}", pathString(response_json));
  value = replaceAll(value, "{run_dir}", jsonString(context, "run_dir"));
  value = replaceAll(value, "{trace_run_dir}", jsonString(context, "trace_run_dir"));
  return value;
}

#ifdef _WIN32
std::wstring widenUtf8(const std::string& text) {
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

int runProcessCommand(
    const std::string& command_line,
    const fs::path& working_directory,
    int timeout_ms,
    const fs::path& trace_dir) {
  appendTrace(trace_dir, "external_process_begin command=" + command_line +
                            " cwd=" + pathString(working_directory));
#ifdef _WIN32
  std::wstring command_w = widenUtf8(command_line);
  std::vector<wchar_t> command_buffer(command_w.begin(), command_w.end());
  command_buffer.push_back(L'\0');
  const fs::path cwd = working_directory.empty() ? fs::path() : fs::absolute(working_directory);
  std::wstring cwd_w = cwd.empty() ? std::wstring() : cwd.wstring();

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const BOOL created = CreateProcessW(
      nullptr,
      command_buffer.data(),
      nullptr,
      nullptr,
      FALSE,
      CREATE_NO_WINDOW,
      nullptr,
      cwd_w.empty() ? nullptr : cwd_w.c_str(),
      &startup,
      &process);
  if (!created) {
    throw std::runtime_error("CreateProcess failed for adapter command: " + command_line);
  }
  const DWORD wait_ms = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE;
  const DWORD wait_result = WaitForSingleObject(process.hProcess, wait_ms);
  if (wait_result == WAIT_TIMEOUT) {
    (void)TerminateProcess(process.hProcess, 1460);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    throw std::runtime_error("Adapter command timeout after " + std::to_string(timeout_ms) +
                             " ms: " + command_line);
  }
  DWORD exit_code = 1;
  (void)GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  appendTrace(trace_dir, "external_process_end exit_code=" + std::to_string(exit_code));
  return static_cast<int>(exit_code);
#else
  (void)timeout_ms;
  const int rc = std::system(command_line.c_str());
  appendTrace(trace_dir, "external_process_end exit_code=" + std::to_string(rc));
  return rc;
#endif
}

json requireArray(const json& value, const std::string& key, const fs::path& source) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_array()) {
    throw std::runtime_error("JSON array '" + key + "' missing in " + pathString(source));
  }
  return value.at(key);
}

std::vector<json> topoSortNodes(const json& nodes_array) {
  std::vector<json> nodes;
  std::set<std::string> known_ids;
  for (const auto& item : nodes_array) {
    if (item.is_object()) {
      const std::string node_id = jsonString(item, "node_id");
      if (node_id.empty()) {
        throw std::runtime_error("Workflow execution_plan contains a node with empty node_id.");
      }
      if (!known_ids.insert(node_id).second) {
        throw std::runtime_error("Workflow execution_plan contains duplicate node_id: " + node_id);
      }
      nodes.push_back(item);
    }
  }
  for (const auto& node : nodes) {
    const std::string node_id = jsonString(node, "node_id");
    if (!node.contains("depends_on") || !node.at("depends_on").is_array()) {
      continue;
    }
    for (const auto& dep : node.at("depends_on")) {
      if (!dep.is_string()) {
        throw std::runtime_error("Workflow dependency for node " + node_id + " is not a string.");
      }
      const std::string dep_id = dep.get<std::string>();
      if (!known_ids.count(dep_id)) {
        throw std::runtime_error("Workflow node " + node_id + " depends on missing node: " + dep_id);
      }
    }
  }
  std::vector<json> sorted;
  std::set<std::string> done;
  while (sorted.size() < nodes.size()) {
    bool progressed = false;
    for (const auto& node : nodes) {
      const std::string node_id = jsonString(node, "node_id");
      if (node_id.empty() || done.count(node_id)) {
        continue;
      }
      bool ready = true;
      if (node.contains("depends_on") && node.at("depends_on").is_array()) {
        for (const auto& dep : node.at("depends_on")) {
          if (dep.is_string() && !done.count(dep.get<std::string>())) {
            ready = false;
            break;
          }
        }
      }
      if (ready) {
        sorted.push_back(node);
        done.insert(node_id);
        progressed = true;
      }
    }
    if (!progressed) {
      std::ostringstream details;
      bool first_node = true;
      for (const auto& node : nodes) {
        const std::string node_id = jsonString(node, "node_id");
        if (node_id.empty() || done.count(node_id)) {
          continue;
        }
        if (!first_node) {
          details << "; ";
        }
        first_node = false;
        details << node_id << " waits_for=[";
        bool first_dep = true;
        if (node.contains("depends_on") && node.at("depends_on").is_array()) {
          for (const auto& dep : node.at("depends_on")) {
            const std::string dep_id = dep.is_string() ? dep.get<std::string>() : "<non-string>";
            if (done.count(dep_id)) {
              continue;
            }
            if (!first_dep) {
              details << ",";
            }
            first_dep = false;
            details << dep_id;
          }
        }
        details << "]";
      }
      throw std::runtime_error(
          "Workflow dependency graph is cyclic or unresolved; fail-fast instead of executing an invalid order. " +
          details.str());
    }
  }
  return sorted;
}

json nodePlanInfo(const json& plan, const std::string& node_id) {
  if (plan.is_object() && plan.contains("nodes") && plan.at("nodes").is_array()) {
    for (const auto& item : plan.at("nodes")) {
      if (jsonString(item, "node_id") == node_id) {
        return item;
      }
    }
  }
  return json::object();
}

json schedulerInfo(const json& scheduler_plan, const std::string& node_id) {
  json info = nodePlanInfo(scheduler_plan, node_id);
  if (info.empty()) {
    info = {{"node_id", node_id}, {"scheduling_level", 0}, {"parallel_group_id", ""}, {"can_run_parallel", false}};
  }
  return info;
}

flightenv::platform::SimulationTimePoint simulationTimePointFromJson(const json& value) {
  flightenv::platform::SimulationTimePoint point;
  point.run_time_s = jsonDouble(value, "run_time_s", 0.0);
  point.tick_index = jsonInt(value, "tick_index", 0);
  point.source_time_s = jsonDouble(value, "source_time_s", point.run_time_s);
  point.stamp_ns = static_cast<long long>(jsonDouble(value, "stamp_ns", 0.0));
  return point;
}

json readyQueueSchedulerEvidence(const flightenv::platform::ReadyQueueScheduler& scheduler) {
  return {
      {"type", "FlightEnvPlatform::ReadyQueueScheduler"},
      {"policy",
       {
           {"max_parallelism", scheduler.policy.max_parallelism},
           {"ready_queue_policy", scheduler.policy.ready_queue_policy},
           {"resource_conflict_policy", scheduler.policy.resource_conflict_policy},
           {"deadline_policy", scheduler.policy.deadline_policy},
           {"default_deadline_s", scheduler.policy.default_deadline_s},
           {"record_timeline", scheduler.policy.record_timeline},
       }},
      {"plan_node_count", scheduler.plan_nodes.size()},
      {"effective_max_parallelism", scheduler.max_parallelism()},
  };
}

json runtimePacketToJson(const flightenv::platform::RuntimePacket& packet);

json threadPoolExecutorEvidence(const flightenv::platform::ThreadPoolExecutorDescriptor& executor) {
  return {
      {"type", "FlightEnvPlatform::ThreadPoolExecutorDescriptor"},
      {"options",
       {
           {"max_workers", executor.options.max_workers},
           {"worker_name_prefix", executor.options.worker_name_prefix},
       }},
  };
}

json runtimePacketArrayFromPortStore(const flightenv::platform::ThreadSafePortStore& port_store) {
  json packets = json::array();
  for (const auto& packet : port_store.snapshot_packets()) {
    packets.push_back(runtimePacketToJson(packet));
  }
  return packets;
}

json portStoreEvidence(const flightenv::platform::ThreadSafePortStore& port_store) {
  const std::vector<flightenv::platform::RuntimePacket> packets = port_store.snapshot_packets();
  std::set<std::string> output_nodes;
  std::set<std::string> ports;
  std::set<std::string> logical_node_ports;
  std::set<std::string> scoped_logical_node_ports;
  for (const auto& packet : packets) {
    if (!packet.node_id.empty()) {
      output_nodes.insert(packet.node_id);
    }
    if (!packet.port_name.empty()) {
      ports.insert(packet.port_name);
    }
    const auto port_id = packet.tags.find("port_id");
    if (!packet.node_id.empty() && port_id != packet.tags.end() && !port_id->second.empty()) {
      logical_node_ports.insert(packet.node_id + "." + port_id->second);
      const auto branch_id = packet.tags.find("branch_id");
      const auto timeline_id = packet.tags.find("timeline_id");
      scoped_logical_node_ports.insert(
          (branch_id == packet.tags.end() ? std::string() : branch_id->second) + "|" +
          (timeline_id == packet.tags.end() ? std::string() : timeline_id->second) + "|" +
          packet.node_id + "." + port_id->second);
    }
  }
  return {
      {"type", "FlightEnvPlatform::ThreadSafePortStore"},
      {"packet_count", packets.size()},
      {"port_count", ports.size()},
      {"node_output_count", output_nodes.size()},
      {"port_packet_count", logical_node_ports.size()},
      {"scoped_port_packet_count", scoped_logical_node_ports.size()},
  };
}

json resourcePayload(const std::map<std::string, json>& resource_locks_by_id, const json& node) {
  json payload = json::object();
  if (!node.contains("resource_refs") || !node.at("resource_refs").is_array()) {
    return payload;
  }
  for (const auto& ref : node.at("resource_refs")) {
    if (!ref.is_string()) {
      continue;
    }
    const std::string id = ref.get<std::string>();
    const auto found = resource_locks_by_id.find(id);
    if (found != resource_locks_by_id.end()) {
      payload[id] = found->second;
    }
  }
  return payload;
}

json loadExternalObservations(const fs::path& path) {
  if (path.empty() || !fs::exists(path)) {
    return json::array();
  }
  const json payload = readJson(path);
  if (payload.is_object() && payload.contains("frames") && payload.at("frames").is_array()) {
    return payload.at("frames");
  }
  if (payload.is_object() && payload.contains("observations") && payload.at("observations").is_array()) {
    return payload.at("observations");
  }
  if (payload.is_array()) {
    return payload;
  }
  if (payload.is_object()) {
    return json::array({payload});
  }
  return json::array();
}

json externalObservationFramePayload(const json& frame) {
  if (frame.is_object() && frame.contains("frame") && frame.at("frame").is_object()) {
    return frame.at("frame");
  }
  return frame;
}

json externalObservationSnapshot(const json& frame, int index, const fs::path& source_path) {
  const json payload = externalObservationFramePayload(frame);
  if (!payload.is_object() || !payload.contains("values") || !payload.at("values").is_array()) {
    return json::object();
  }
  json snapshot = {
      {"frame_contract", "ObservationSnapshot.v1"},
      {"contract_id", "flightenv.external_sensor_observation.v1"},
      {"port_id", "observation.actual"},
      {"representation", "inline_sensor_vector"},
      {"source", payload.value("source", std::string("external_observation_stream"))},
      {"source_stream_path", pathString(source_path)},
      {"frame_index", index},
      {"sample_time_s", jsonDouble(payload, "sample_time_s", jsonDouble(payload, "time_s", index))},
      {"values", payload.at("values")},
      {"sensor_count", payload.value("sensor_count", static_cast<int>(payload.at("values").size()))},
  };
  if (payload.contains("sensor_ids") && payload.at("sensor_ids").is_array()) {
    snapshot["sensor_ids"] = payload.at("sensor_ids");
  }
  if (payload.contains("selected_state")) {
    snapshot["selected_state"] = payload.at("selected_state");
  }
  if (payload.contains("status")) {
    snapshot["status"] = payload.at("status");
  }
  return snapshot;
}

json externalObservationSeed(const json& observations, int iteration_index, const fs::path& source_path) {
  if (!observations.is_array() || observations.empty()) {
    return json::object();
  }
  const int index = std::min<int>(iteration_index, static_cast<int>(observations.size()) - 1);
  const json raw_frame = observations.at(static_cast<std::size_t>(index));
  const json frame = externalObservationFramePayload(raw_frame);
  const json actual_snapshot = externalObservationSnapshot(raw_frame, index, source_path);
  json seed = {
      {"__external_observation__", frame},
      {"external_observation",
       {
           {"source", "external_observation_stream"},
           {"stream_path", pathString(source_path)},
           {"frame_index", index},
           {"sample_time_s", jsonDouble(frame, "sample_time_s", jsonDouble(frame, "time_s", index))},
           {"frame", frame},
       }},
  };
  if (!actual_snapshot.empty()) {
    seed["observation.actual"] = actual_snapshot;
    seed["external_observation_actual"] = actual_snapshot;
  }
  if (frame.is_object()) {
    if (frame.contains("sensor_count")) {
      seed["external_observation"]["sensor_count"] = frame.at("sensor_count");
    } else if (frame.contains("sensors") && frame.at("sensors").is_array()) {
      seed["external_observation"]["sensor_count"] = frame.at("sensors").size();
    } else if (frame.contains("observations") && frame.at("observations").is_array()) {
      seed["external_observation"]["sensor_count"] = frame.at("observations").size();
    }
  }
  return seed;
}

json recordExternalObservationSample(
    RuntimePortSampleBuffer& sample_buffer,
    const RuntimeEvent& event,
    const fs::path& source_path) {
  const int frame_index = event.payload.value("frame_index", event.iteration_index);
  const json frame = event.payload.value("frame", json::object());
  const json actual_snapshot = externalObservationSnapshot(frame, frame_index, source_path);
  const json value = actual_snapshot.empty() ? frame : actual_snapshot;
  const double sample_time_s = event.payload.value("sample_time_s", event.event_time_s);
  const json time_info = {
      {"public_output_time_s", sample_time_s},
      {"public_output_time_ns", event.event_time.nanoseconds},
      {"runtime_event_time_s", event.event_time_s},
      {"runtime_event_time_ns", event.event_time.nanoseconds},
      {"input_event_id", event.event_id},
      {"source", event.payload.value("source", std::string("external_observation_stream"))},
      {"source_stream_path", pathString(source_path)},
      {"frame_index", frame_index},
  };

  RuntimePortSample node_sample;
  node_sample.channel_id = RuntimePortSampleBuffer::nodeChannel("external_observation");
  node_sample.source_node_id = "external_observation";
  node_sample.time_s = sample_time_s;
  node_sample.iteration_index = frame_index;
  node_sample.value = frame;
  node_sample.time_info = time_info;
  sample_buffer.recordSample(node_sample);

  RuntimePortSample port_sample;
  port_sample.channel_id = RuntimePortSampleBuffer::portChannel("external_observation", "observation.actual");
  port_sample.source_node_id = "external_observation";
  port_sample.source_port_id = "observation.actual";
  port_sample.time_s = sample_time_s;
  port_sample.iteration_index = frame_index;
  port_sample.value = value;
  port_sample.time_info = time_info;
  sample_buffer.recordSample(port_sample);

  return {
      {"sample_recorded", true},
      {"source_node_id", "external_observation"},
      {"source_port_id", "observation.actual"},
      {"channel_id", port_sample.channel_id},
      {"node_channel_id", node_sample.channel_id},
      {"sample_time_s", sample_time_s},
      {"sample_time_ns", event.event_time.nanoseconds},
      {"frame_index", frame_index},
      {"value_kind", value.type_name()},
      {"has_actual_snapshot", !actual_snapshot.empty()},
  };
}

json recurrentSeedFromOutputs(const json& outputs, int iteration_index) {
  json seed = {
      {"__previous_iteration__", {{"loop_iteration_index", iteration_index}, {"outputs", outputs}}},
  };
  for (auto it = outputs.begin(); it != outputs.end(); ++it) {
    const json& node_output = it.value();
    if (!node_output.is_object()) {
      continue;
    }
    const json ports = node_output.value("outputs", json::object());
    if (ports.is_object() && ports.contains("state.posterior")) {
      seed["__previous_posterior_state__"] = {
          {"node_id", it.key()},
          {"outputs", ports},
      };
      return seed;
    }
  }
  return seed;
}

json seedFromRuntimeOutputs(const fs::path& path) {
  if (path.empty() || !fs::exists(path)) {
    return json::object();
  }
  const json payload = readJson(path);
  const json outputs = payload.value("outputs", json::object());
  json seed = recurrentSeedFromOutputs(outputs, jsonInt(payload, "last_loop_iteration_index", 0));
  seed["__seed_runtime_outputs__"] = {
      {"path", pathString(path)},
      {"run_id", jsonString(payload, "run_id")},
      {"workflow_id", jsonString(payload, "workflow_id")},
  };
  return seed;
}

flightenv::platform::RuntimePacket buildRuntimePacket(
    const std::string& run_id,
    const std::string& object_id,
    const std::string& branch_id,
    const std::string& timeline_id,
    const std::string& node_id,
    const json& execute_result,
    const json& time_info,
    const json& data_plane_entries) {
  const json output_time = time_info.value("output_time_point", time_info.value("time_point", json::object()));
  json output_refs = json::array();
  json first_output_ref = json::object();
  if (data_plane_entries.is_array()) {
    for (const auto& entry : data_plane_entries) {
      if (!entry.is_object() || jsonString(entry, "direction") != "output") {
        continue;
      }
      output_refs.push_back(entry);
      if (first_output_ref.empty()) {
        first_output_ref = entry;
      }
    }
  }
  flightenv::platform::RuntimePacket packet;
  packet.run_id = run_id;
  packet.object_id = object_id;
  packet.port_name = "node." + node_id + ".output";
  packet.node_id = node_id;
  packet.time_point = simulationTimePointFromJson(output_time);
  packet.producer_node = node_id;
  packet.payload_kind = "inline_summary_json";
  packet.inline_payload_json = json{
      {"status", jsonString(execute_result, "status", "ok")},
      {"output_ports", execute_result.value("outputs", json::object())},
      {"data_plane_refs", output_refs},
      {"summary", summarizeJson(execute_result)},
  }.dump(-1, ' ', false, json::error_handler_t::replace);
  packet.payload_ref = "runtime_node_snapshot.json";
  if (first_output_ref.is_object()) {
    packet.contract_id = jsonString(first_output_ref, "contract_id");
    packet.typed_schema_id = jsonString(first_output_ref, "typed_schema_id");
    packet.typed_dto_name = jsonString(first_output_ref, "typed_dto_name");
    packet.typed_payload_ref = jsonString(first_output_ref, "typed_payload_ref");
    packet.typed_buffer_ref = jsonString(first_output_ref, "typed_buffer_ref");
    packet.buffer_layout_id = jsonString(first_output_ref, "buffer_layout_id",
                                         jsonString(first_output_ref, "layout_ref"));
    packet.buffer_bytes = jsonUInt64(first_output_ref, "buffer_bytes", 0);
    packet.zero_copy_eligible = jsonBool(first_output_ref, "zero_copy_eligible", false);
  }
  packet.tags["created_at_utc"] = nowUtcIso();
  packet.tags["branch_id"] = branch_id;
  packet.tags["timeline_id"] = timeline_id;
  packet.tags["data_plane_ref_count"] = std::to_string(output_refs.size());
  return packet;
}

json runtimePacketToJson(const flightenv::platform::RuntimePacket& packet) {
  json payload = json::object();
  try {
    payload = json::parse(packet.inline_payload_json);
  } catch (...) {
    payload = {{"raw", packet.inline_payload_json}};
  }
  return {
      {"run_id", packet.run_id},
      {"object_id", packet.object_id},
      {"branch_id", packet.tags.count("branch_id") ? packet.tags.at("branch_id") : ""},
      {"timeline_id", packet.tags.count("timeline_id") ? packet.tags.at("timeline_id") : ""},
      {"port_name", packet.port_name},
      {"node_id", packet.node_id},
      {"time_s", packet.time_point.run_time_s},
      {"tick_index", packet.time_point.tick_index},
      {"source_time_s", packet.time_point.source_time_s},
      {"stamp_ns", packet.time_point.stamp_ns},
      {"version", packet.version},
      {"producer_node", packet.producer_node},
      {"payload_kind", packet.payload_kind},
      {"payload", payload},
      {"payload_ref", packet.payload_ref},
      {"contract_id", packet.contract_id},
      {"typed_schema_id", packet.typed_schema_id},
      {"typed_dto_name", packet.typed_dto_name},
      {"typed_payload_ref", packet.typed_payload_ref},
      {"typed_buffer_ref", packet.typed_buffer_ref},
      {"buffer_layout_id", packet.buffer_layout_id},
      {"buffer_bytes", packet.buffer_bytes},
      {"zero_copy_eligible", packet.zero_copy_eligible},
      {"checksum", packet.checksum},
      {"tags", packet.tags},
      {"created_at_utc", packet.tags.count("created_at_utc") ? packet.tags.at("created_at_utc") : nowUtcIso()},
  };
}

json makeDataPlaneEntries(
    const json& data_plane_info,
    const json& input_payload,
    const json& output_payload,
    const json& time_info,
    int iteration_index) {
  return RuntimeMaterialization::makeDataPlaneEntries(
      data_plane_info,
      input_payload,
      output_payload,
      time_info,
      iteration_index);
}

#ifdef _WIN32
SRWLOCK g_dll_call_lock = SRWLOCK_INIT;

class DllCallGuard {
 public:
  DllCallGuard() {
    AcquireSRWLockExclusive(&g_dll_call_lock);
  }
  ~DllCallGuard() {
    ReleaseSRWLockExclusive(&g_dll_call_lock);
  }
  DllCallGuard(const DllCallGuard&) = delete;
  DllCallGuard& operator=(const DllCallGuard&) = delete;
};
#else
std::mutex g_dll_call_mutex;

class DllCallGuard {
 public:
  DllCallGuard() : lock_(g_dll_call_mutex) {}
 private:
  std::lock_guard<std::mutex> lock_;
};
#endif

struct RuntimeTypedInputViewHolder {
  RuntimeTypedBufferAllocation allocation;
  flightenv::platform::AdapterTypedBufferView view{};
  std::string buffer_id;
  std::string node_id;
  std::string port_id;
  std::string schema_id;
  std::string dto_name;
  std::string layout_id;
  std::string format;
};

class RuntimeTypedInputRetainScope {
 public:
  explicit RuntimeTypedInputRetainScope(const std::vector<RuntimeTypedInputViewHolder>& holders) {
    buffer_ids_.reserve(holders.size());
    for (const RuntimeTypedInputViewHolder& holder : holders) {
      if (!holder.buffer_id.empty()) {
        buffer_ids_.push_back(holder.buffer_id);
      }
    }
  }

  ~RuntimeTypedInputRetainScope() {
    RuntimeTypedBufferStore& store = RuntimeTypedBufferStore::instance();
    for (const std::string& buffer_id : buffer_ids_) {
      (void)store.release(buffer_id);
    }
  }

  RuntimeTypedInputRetainScope(const RuntimeTypedInputRetainScope&) = delete;
  RuntimeTypedInputRetainScope& operator=(const RuntimeTypedInputRetainScope&) = delete;

 private:
  std::vector<std::string> buffer_ids_;
};

void collectTypedBufferRefs(const json& value, std::vector<json>& refs) {
  if (value.is_object()) {
    if (value.contains("typed_buffer_ref") && value.at("typed_buffer_ref").is_object()) {
      refs.push_back(value.at("typed_buffer_ref"));
    }
    for (const auto& item : value.items()) {
      collectTypedBufferRefs(item.value(), refs);
    }
    return;
  }
  if (value.is_array()) {
    for (const auto& item : value) {
      collectTypedBufferRefs(item, refs);
    }
  }
}

std::vector<RuntimeTypedInputViewHolder> buildRuntimeTypedInputViews(const json& payload) {
  std::vector<json> refs;
  collectTypedBufferRefs(payload, refs);
  std::vector<RuntimeTypedInputViewHolder> holders;
  holders.reserve(refs.size());
  RuntimeTypedBufferStore& store = RuntimeTypedBufferStore::instance();
  for (const json& ref : refs) {
    const std::string buffer_id = jsonString(ref, "buffer_id");
    RuntimeTypedBufferAllocation allocation = store.retain(buffer_id);
    if (buffer_id.empty() || !allocation.bytes || allocation.bytes->empty()) {
      continue;
    }
    RuntimeTypedInputViewHolder holder;
    holder.allocation = std::move(allocation);
    holder.buffer_id = buffer_id;
    holder.node_id = jsonString(ref, "node_id");
    holder.port_id = jsonString(ref, "port_id");
    holder.schema_id = jsonString(ref, "schema_id");
    holder.dto_name = jsonString(ref, "dto_name");
    holder.layout_id = jsonString(ref, "layout_id", holder.schema_id);
    holder.format = jsonString(ref, "format", "fe_typed_dto_binary.v1");
    holders.push_back(std::move(holder));
  }
  for (RuntimeTypedInputViewHolder& holder : holders) {
    holder.view.buffer_id = holder.buffer_id.c_str();
    holder.view.node_id = holder.node_id.c_str();
    holder.view.port_id = holder.port_id.c_str();
    holder.view.data = holder.allocation.bytes->data();
    holder.view.byte_size = static_cast<std::uint64_t>(holder.allocation.bytes->size());
    holder.view.dtype = holder.allocation.dtype;
    holder.view.rank = holder.allocation.rank;
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(holder.view.rank, 8u); ++i) {
      holder.view.shape[i] = holder.allocation.shape[i];
    }
    holder.view.schema_id = holder.schema_id.c_str();
    holder.view.dto_name = holder.dto_name.c_str();
    holder.view.layout_id = holder.layout_id.c_str();
    holder.view.format = holder.format.c_str();
    holder.view.flags = flightenv::platform::AdapterTypedBufferFlagRuntimeOwned |
                        flightenv::platform::AdapterTypedBufferFlagReadOnly;
  }
  return holders;
}

json dataPlanePortSpec(const json& data_plane_info, const std::string& port_id) {
  const json outputs = data_plane_info.value("outputs", json::array());
  if (!outputs.is_array()) {
    return json::object();
  }
  for (const json& spec : outputs) {
    if (jsonString(spec, "port_id") == port_id) {
      return spec;
    }
  }
  return json::object();
}

#ifdef _WIN32
void addDllDirectoryIfExists(const fs::path& path) {
  if (!path.empty() && fs::exists(path)) {
    (void)AddDllDirectory(path.wstring().c_str());
  }
}

void configureDllSearchPath(const fs::path& library_path, const NativeWorkflowOptions& options) {
  static std::once_flag once;
  std::call_once(once, [&]() {
    (void)SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    addDllDirectoryIfExists(library_path.parent_path());
    addDllDirectoryIfExists(options.workspace_root / "_deps/workspace/x64/Release");
    addDllDirectoryIfExists(options.workspace_root / "_deps/bin");
    addDllDirectoryIfExists(options.workspace_root / "_deps/bin/legacy_support_runtime");
    addDllDirectoryIfExists(options.workspace_root / "_deps/bin/third_party_runtime");
  });
}
#endif

class DllAbiAdapterSession final : public IAdapterSession {
 public:
  DllAbiAdapterSession(
      json node,
      json operator_snapshot,
      json model_binding,
      json registry_entry,
      NativeWorkflowOptions options,
      fs::path trace_dir)
      : node_(std::move(node)),
        operator_snapshot_(std::move(operator_snapshot)),
        model_binding_(std::move(model_binding)),
        registry_entry_(std::move(registry_entry)),
        options_(std::move(options)),
        trace_dir_(std::move(trace_dir)) {
    library_path_ = fs::absolute(resolveTemplatePath(jsonString(registry_entry_, "library"), options_));
    export_prefix_ = jsonString(registry_entry_, "export_prefix", "flightenv_adapter");
    appendTrace(trace_dir_, "dll_session_construct_begin node=" + jsonString(node_, "node_id") +
                            " adapter=" + jsonString(node_, "adapter_id") +
                            " library=" + pathString(library_path_));
    loadLibrary();
    appendTrace(trace_dir_, "dll_session_load_library_end node=" + jsonString(node_, "node_id") +
                            " adapter=" + jsonString(node_, "adapter_id"));
    createHandle();
    appendTrace(trace_dir_, "dll_session_create_handle_end node=" + jsonString(node_, "node_id") +
                            " adapter=" + jsonString(node_, "adapter_id"));
  }

  ~DllAbiAdapterSession() override {
    shutdown();
  }

  std::string protocol() const override {
    return execute_typed_v2_ ? "dll_abi.v1+typed_abi.v2" : "dll_abi.v1";
  }

  json summary() const override {
    return {
        {"node_id", jsonString(node_, "node_id")},
        {"operator_id", jsonString(node_, "operator_id")},
        {"adapter_id", jsonString(node_, "adapter_id")},
        {"protocol", protocol()},
        {"session_contract", kAdapterSessionContract},
        {"library", pathString(library_path_)},
        {"typed_abi_version", execute_typed_v2_ ? flightenv::platform::kAdapterTypedAbiVersion : ""},
        {"typed_execute_v2_available", execute_typed_v2_ != nullptr},
        {"typed_result_release_v2_available", release_typed_result_v2_ != nullptr},
        {"zero_copy_mode", runtimeZeroCopyModeName(
                               resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode))},
        {"typed_buffer_persistence", runtimeTypedBufferPersistenceName(
                                         resolveRuntimeTypedBufferPersistence(
                                             options_.typed_buffer_persistence))},
        {"prepared", prepared_},
        {"handle_created", handle_ != nullptr},
        {"counters", counters_},
        {"session_mode", "native_in_process_persistent"},
    };
  }

  void shutdown() override {
    if (!handle_) {
      return;
    }
    try {
      const json context = last_context_.empty() ? json::object() : last_context_;
      (void)call("shutdown", context, json::object());
    } catch (...) {
    }
    if (destroy_) {
      DllCallGuard lock;
      (void)destroy_(handle_);
    }
    handle_ = nullptr;
#ifdef _WIN32
    if (module_) {
      (void)FreeLibrary(module_);
      module_ = nullptr;
    }
#endif
    prepared_ = false;
  }

  json describe(const json& context, const json& payload) override {
    return call("describe", context, payload);
  }

  json resolve(const json& context, const json& payload) override {
    return call("resolve", context, payload);
  }

  json initialize(const json& context, const json& payload) override {
    return call("initialize", context, payload);
  }

  json warmup(const json& context, const json& payload) override {
    return call("warmup", context, payload);
  }

  json execute(const json& context, const json& payload) override {
    const RuntimeZeroCopyExecuteDecision decision = zeroCopyExecuteDecision(payload);
    if (decision.fail_fast) {
      throw std::runtime_error(decision.message);
    }
    if (decision.use_typed_execute) {
      return callTypedExecute(context, payload, decision);
    }
    json result = call("execute", context, payload);
    result["runtime_zero_copy_policy"] = runtimeZeroCopyExecuteDecisionToJson(decision);
    return result;
  }

  json snapshot(const json& context, const json& payload) override {
    return call("snapshot", context, payload);
  }

  json flush(const json& context, const json& payload) override {
    return call("flush", context, payload);
  }

  bool prepared() const {
    return prepared_;
  }

  void setPrepared(bool value) {
    prepared_ = value;
  }

 private:
  using CreateFn = int (*)(void** handle);
  using DestroyFn = int (*)(void* handle);
  using JsonCallFn = int (*)(
      void* handle,
      const char* request_json,
      char* response_json,
      std::uint64_t* response_json_size);

  void loadLibrary() {
#ifdef _WIN32
    const fs::path dll_dir = library_path_.parent_path();
    appendTrace(trace_dir_, "dll_load_begin library=" + pathString(library_path_));
    configureDllSearchPath(library_path_, options_);
    if (fs::exists(dll_dir)) {
      (void)SetDllDirectoryW(dll_dir.wstring().c_str());
    }
    HMODULE module = LoadLibraryExW(
        library_path_.wstring().c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!module) {
      module = LoadLibraryW(library_path_.wstring().c_str());
    }
    if (!module) {
      throw std::runtime_error("LoadLibrary failed for adapter DLL: " + pathString(library_path_));
    }
    appendTrace(trace_dir_, "dll_load_end library=" + pathString(library_path_));
    module_ = module;
    create_ = reinterpret_cast<CreateFn>(GetProcAddress(module, (export_prefix_ + "_create_v1").c_str()));
    destroy_ = reinterpret_cast<DestroyFn>(GetProcAddress(module, (export_prefix_ + "_destroy_v1").c_str()));
    describe_ = reinterpret_cast<JsonCallFn>(GetProcAddress(module, (export_prefix_ + "_describe_v1").c_str()));
    resolve_ = reinterpret_cast<JsonCallFn>(GetProcAddress(module, (export_prefix_ + "_resolve_v1").c_str()));
    initialize_ = reinterpret_cast<JsonCallFn>(GetProcAddress(module, (export_prefix_ + "_initialize_v1").c_str()));
    warmup_ = reinterpret_cast<JsonCallFn>(GetProcAddress(module, (export_prefix_ + "_warmup_v1").c_str()));
    execute_ = reinterpret_cast<JsonCallFn>(GetProcAddress(module, (export_prefix_ + "_execute_v1").c_str()));
    execute_typed_v2_ = reinterpret_cast<flightenv::platform::AdapterExecuteTypedV2Fn>(
        GetProcAddress(module, (export_prefix_ + "_execute_typed_v2").c_str()));
    release_typed_result_v2_ = reinterpret_cast<flightenv::platform::AdapterReleaseTypedResultV2Fn>(
        GetProcAddress(module, (export_prefix_ + "_release_typed_result_v2").c_str()));
    snapshot_ = reinterpret_cast<JsonCallFn>(GetProcAddress(module, (export_prefix_ + "_snapshot_v1").c_str()));
    flush_ = reinterpret_cast<JsonCallFn>(GetProcAddress(module, (export_prefix_ + "_flush_v1").c_str()));
    shutdown_ = reinterpret_cast<JsonCallFn>(GetProcAddress(module, (export_prefix_ + "_shutdown_v1").c_str()));
    if (!create_ || !destroy_ || !describe_ || !resolve_ || !initialize_ || !warmup_ || !execute_ ||
        !snapshot_ || !flush_ || !shutdown_) {
      throw std::runtime_error("Adapter DLL missing dll_abi.v1 exports: " + pathString(library_path_));
    }
#else
    throw std::runtime_error("dll_abi.v1 adapter is only implemented for Windows in this host.");
#endif
  }

  void createHandle() {
    void* handle = nullptr;
    appendTrace(trace_dir_, "dll_create_handle_begin node=" + jsonString(node_, "node_id") +
                            " adapter=" + jsonString(node_, "adapter_id"));
    {
      DllCallGuard lock;
      appendTrace(trace_dir_, "dll_create_handle_lock_acquired node=" + jsonString(node_, "node_id") +
                              " adapter=" + jsonString(node_, "adapter_id"));
      const int status = create_(&handle);
      if (status != 0 || handle == nullptr) {
        throw std::runtime_error("DLL adapter create failed: " + jsonString(node_, "adapter_id"));
      }
    }
    handle_ = handle;
  }

  JsonCallFn fnForEvent(const std::string& event) const {
    if (event == "describe") return describe_;
    if (event == "resolve") return resolve_;
    if (event == "initialize") return initialize_;
    if (event == "warmup") return warmup_;
    if (event == "execute") return execute_;
    if (event == "snapshot") return snapshot_;
    if (event == "flush") return flush_;
    if (event == "shutdown") return shutdown_;
    throw std::runtime_error("Unknown adapter event: " + event);
  }

  RuntimeZeroCopyExecuteDecision zeroCopyExecuteDecision(const json& payload) const {
    const json data_plane_info =
        payload.is_object() ? payload.value("data_plane", json::object()) : json::object();
    return decideRuntimeZeroCopyExecute(
        data_plane_info,
        payload,
        options_.runtime_zero_copy_mode,
        true,
        execute_typed_v2_ != nullptr && release_typed_result_v2_ != nullptr,
        jsonString(node_, "node_id"),
        jsonString(node_, "adapter_id"));
  }

  json callTypedExecute(
      const json& context,
      const json& payload,
      const RuntimeZeroCopyExecuteDecision& zero_copy_decision) {
    if (!handle_) {
      createHandle();
    }
    counters_["execute"] = counters_.value("execute", 0) + 1;
    last_context_ = context;
    const json data_plane_info = payload.value("data_plane", json::object());
    json request = {
        {"schema_version", "flightenv.platform.adapter_call.v1"},
        {"event", "execute"},
        {"run_id", jsonString(context, "run_id")},
        {"workflow_id", jsonString(context, "workflow_id")},
        {"object_id", jsonString(context, "object_id")},
        {"run_dir", jsonString(context, "run_dir")},
        {"loop_iteration_index", jsonInt(context, "loop_iteration_index", 0)},
        {"node", context.value("node", node_)},
        {"operator_snapshot", operator_snapshot_},
        {"resource_locks", context.value("resource_locks", json::object())},
        {"model_binding", model_binding_},
        {"payload", payload},
    };

    std::vector<RuntimeTypedInputViewHolder> input_holders = buildRuntimeTypedInputViews(payload);
    RuntimeTypedInputRetainScope input_retain_scope(input_holders);
    std::vector<flightenv::platform::AdapterTypedBufferView> input_views;
    input_views.reserve(input_holders.size());
    for (const RuntimeTypedInputViewHolder& holder : input_holders) {
      input_views.push_back(holder.view);
    }

    const fs::path trace_dir = jsonString(context, "trace_run_dir", jsonString(context, "run_dir"));
    appendTrace(trace_dir, "adapter_typed_call_begin node=" + jsonString(node_, "node_id") +
                                " adapter=" + jsonString(node_, "adapter_id") +
                                " input_buffers=" + std::to_string(input_views.size()));
    const std::string request_text = request.dump(-1, ' ', false, json::error_handler_t::replace);
    flightenv::platform::AdapterTypedExecuteRequestV2 typed_request{};
    typed_request.abi_version = flightenv::platform::kAdapterTypedAbiVersion;
    typed_request.request_json = request_text.c_str();
    typed_request.inputs = input_views.empty() ? nullptr : input_views.data();
    typed_request.input_count = static_cast<std::uint64_t>(input_views.size());
    RuntimeTypedBufferAllocatorContext allocator_context;
    allocator_context.store = &RuntimeTypedBufferStore::instance();
    allocator_context.run_dir = fs::path(jsonString(context, "run_dir"));
    allocator_context.persistence_mode =
        resolveRuntimeTypedBufferPersistence(options_.typed_buffer_persistence);
    typed_request.allocator = makeRuntimeTypedBufferAllocator(allocator_context);

    flightenv::platform::AdapterTypedExecuteResultV2 typed_result{};
    struct ResultReleaseGuard {
      void* handle = nullptr;
      flightenv::platform::AdapterReleaseTypedResultV2Fn release = nullptr;
      flightenv::platform::AdapterTypedExecuteResultV2* result = nullptr;
      ~ResultReleaseGuard() {
        if (handle && release && result) {
          (void)release(handle, result);
        }
      }
    } result_guard{handle_, release_typed_result_v2_, &typed_result};

    const auto start = std::chrono::steady_clock::now();
    flightenv::platform::AdapterAbiStatus status = flightenv::platform::AdapterAbiStatus::FatalError;
    {
      DllCallGuard lock;
      status = execute_typed_v2_(handle_, &typed_request, &typed_result);
    }
    const int duration_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    const std::string response_text = typed_result.response_json ? std::string(typed_result.response_json) : "{}";
    if (status != flightenv::platform::AdapterAbiStatus::Ok) {
      throw std::runtime_error("DLL adapter typed execute failed: adapter=" + jsonString(node_, "adapter_id") +
                               " status=" + std::to_string(static_cast<int>(status)) +
                               " reason=" + response_text.substr(0, 500));
    }

    json parsed = json::parse(response_text);
    const std::string status_text = jsonString(parsed, "status", "ok");
    if (status_text != "ok") {
      throw std::runtime_error("DLL adapter typed execute returned status '" + status_text + "' for " +
                               jsonString(node_, "adapter_id") + ": " + jsonString(parsed, "reason"));
    }
    json result = parsed.value("result", json::object());
    if (!result.is_object()) {
      throw std::runtime_error("DLL adapter typed execute result must be an object: " + jsonString(node_, "adapter_id"));
    }
    if (!result.contains("outputs") || !result.at("outputs").is_object()) {
      result["outputs"] = json::object();
    }
    if (typed_result.output_count > 0 && typed_result.outputs == nullptr) {
      throw std::runtime_error("DLL adapter typed execute returned output_count without output descriptors");
    }

    RuntimeTypedBufferStore& store = RuntimeTypedBufferStore::instance();
    for (std::uint64_t i = 0; i < typed_result.output_count; ++i) {
      const flightenv::platform::AdapterTypedBufferView& output = typed_result.outputs[i];
      const std::string buffer_id = output.buffer_id ? output.buffer_id : "";
      const std::string port_id = output.port_id ? output.port_id : ("output." + std::to_string(i));
      if (buffer_id.empty() || port_id.empty()) {
        continue;
      }
      (void)store.refreshShadowArtifact(buffer_id);
      json ref = store.refForBuffer(buffer_id);
      if (!ref.is_object() || ref.empty()) {
        ref = json{{"buffer_id", buffer_id},
                   {"uri", "runtime://typed-buffer/" + buffer_id},
                   {"schema_id", output.schema_id ? output.schema_id : ""},
                   {"dto_name", output.dto_name ? output.dto_name : ""},
                   {"layout_id", output.layout_id ? output.layout_id : ""},
                   {"format", output.format ? output.format : ""},
                   {"byte_size", output.byte_size},
                   {"zero_copy_eligible", true},
                   {"storage", "runtime_owned_memory"}};
      }
      json& port_payload = result["outputs"][port_id];
      if (!port_payload.is_object()) {
        port_payload = json::object();
      }
      port_payload["typed_buffer_ref"] = ref;
      port_payload["typed_schema_id"] = output.schema_id ? output.schema_id : "";
      port_payload["typed_dto_name"] = output.dto_name ? output.dto_name : "";
      port_payload["typed_buffer_format"] = output.format ? output.format : "";
      port_payload["adapter_typed_abi_v2"] = true;
      const json port_spec = dataPlanePortSpec(data_plane_info, port_id);
      if (port_spec.is_object() && !port_spec.empty()) {
        port_payload["contract_id"] = jsonString(port_spec, "contract_id");
        port_payload["frame_contract"] = jsonString(port_spec, "frame_contract");
      }
    }

    result["adapter_protocol"] = protocol();
    result["adapter_typed_abi_v2"] = true;
    result["adapter_typed_input_count"] = input_views.size();
    result["adapter_typed_output_count"] = typed_result.output_count;
    result["runtime_zero_copy_policy"] =
        runtimeZeroCopyExecuteDecisionToJson(zero_copy_decision);
    result["library"] = pathString(library_path_);
    result["duration_ms"] = duration_ms;
    result["thread_safety"] = jsonString(registry_entry_, "thread_safety", "host_serialized");
    result["session_mode"] = "native_in_process_persistent";
    appendTrace(trace_dir, "adapter_typed_call_end node=" + jsonString(node_, "node_id") +
                              " adapter=" + jsonString(node_, "adapter_id") +
                              " duration_ms=" + std::to_string(duration_ms) +
                              " output_buffers=" + std::to_string(typed_result.output_count));
    return result;
  }

  json call(const std::string& event, const json& context, const json& payload) {
    if (!handle_) {
      createHandle();
    }
    counters_[event] = counters_.value(event, 0) + 1;
    last_context_ = context;
    json request = {
        {"schema_version", "flightenv.platform.adapter_call.v1"},
        {"event", event},
        {"run_id", jsonString(context, "run_id")},
        {"workflow_id", jsonString(context, "workflow_id")},
        {"object_id", jsonString(context, "object_id")},
        {"run_dir", jsonString(context, "run_dir")},
        {"loop_iteration_index", jsonInt(context, "loop_iteration_index", 0)},
        {"node", context.value("node", node_)},
        {"operator_snapshot", operator_snapshot_},
        {"resource_locks", context.value("resource_locks", json::object())},
        {"model_binding", model_binding_},
        {"payload", payload},
    };
    const fs::path trace_dir = jsonString(context, "trace_run_dir", jsonString(context, "run_dir"));
    appendTrace(trace_dir, "adapter_call_begin node=" + jsonString(node_, "node_id") +
                                " adapter=" + jsonString(node_, "adapter_id") +
                                " event=" + event +
                                " session_prepared=" + (prepared_ ? std::string("true") : std::string("false")));
    const std::string request_text = request.dump(-1, ' ', false, json::error_handler_t::replace);
    std::uint64_t response_size = 4ULL * 1024ULL * 1024ULL;
    std::vector<char> response(static_cast<std::size_t>(response_size) + 1U, '\0');
    const auto start = std::chrono::steady_clock::now();
    auto fn = fnForEvent(event);
    int status_code = 0;
    {
      DllCallGuard lock;
      status_code = fn(handle_, request_text.c_str(), response.data(), &response_size);
      if (status_code == static_cast<int>(flightenv::platform::AdapterAbiStatus::BufferTooSmall)) {
        response.assign(static_cast<std::size_t>(response_size) + 1U, '\0');
        status_code = fn(handle_, request_text.c_str(), response.data(), &response_size);
      }
    }
    const int duration_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    if (status_code != static_cast<int>(flightenv::platform::AdapterAbiStatus::Ok)) {
      std::string reason(response.data(), response.data() + std::min<std::uint64_t>(response_size, 500));
      throw std::runtime_error("DLL adapter event failed: adapter=" + jsonString(node_, "adapter_id") +
                               " event=" + event + " status=" + std::to_string(status_code) +
                               " reason=" + reason);
    }
    std::string response_text(response.data(), response.data() + response_size);
    json parsed = json::parse(response_text);
    const std::string status_text = jsonString(parsed, "status", "ok");
    if (status_text != "ok") {
      throw std::runtime_error("DLL adapter returned status '" + status_text + "' for " +
                               jsonString(node_, "adapter_id") + "/" + event +
                               ": " + jsonString(parsed, "reason"));
    }
    json result = parsed.value("result", json::object());
    if (!result.is_object()) {
      throw std::runtime_error("DLL adapter result must be an object: " + jsonString(node_, "adapter_id"));
    }
    result["adapter_protocol"] = protocol();
    result["library"] = pathString(library_path_);
    result["duration_ms"] = duration_ms;
    result["thread_safety"] = jsonString(registry_entry_, "thread_safety", "host_serialized");
    result["session_mode"] = "native_in_process_persistent";
    appendTrace(trace_dir, "adapter_call_end node=" + jsonString(node_, "node_id") +
                              " adapter=" + jsonString(node_, "adapter_id") +
                              " event=" + event +
                              " duration_ms=" + std::to_string(duration_ms));
    return result;
  }

  json node_;
  json operator_snapshot_;
  json model_binding_;
  json registry_entry_;
  NativeWorkflowOptions options_;
  fs::path library_path_;
  fs::path trace_dir_;
  std::string export_prefix_ = "flightenv_adapter";
  json counters_ = json::object();
  bool prepared_ = false;
  json last_context_ = json::object();

#ifdef _WIN32
  HMODULE module_ = nullptr;
#endif
  CreateFn create_ = nullptr;
  DestroyFn destroy_ = nullptr;
  JsonCallFn describe_ = nullptr;
  JsonCallFn resolve_ = nullptr;
  JsonCallFn initialize_ = nullptr;
  JsonCallFn warmup_ = nullptr;
  JsonCallFn execute_ = nullptr;
  flightenv::platform::AdapterExecuteTypedV2Fn execute_typed_v2_ = nullptr;
  flightenv::platform::AdapterReleaseTypedResultV2Fn release_typed_result_v2_ = nullptr;
  JsonCallFn snapshot_ = nullptr;
  JsonCallFn flush_ = nullptr;
  JsonCallFn shutdown_ = nullptr;
  void* handle_ = nullptr;
};

class RecordingAdapterSession final : public IAdapterSession {
 public:
  explicit RecordingAdapterSession(json node) : node_(std::move(node)) {}

  std::string protocol() const override {
    return "recording.v1";
  }

  json summary() const override {
    return {
        {"node_id", jsonString(node_, "node_id")},
        {"operator_id", jsonString(node_, "operator_id")},
        {"adapter_id", jsonString(node_, "adapter_id")},
        {"protocol", protocol()},
        {"session_contract", kAdapterSessionContract},
        {"session_mode", "native_recording_noop"},
        {"counters", counters_},
    };
  }

  void shutdown() override {}

  json describe(const json&, const json&) override { return call("describe"); }
  json resolve(const json&, const json&) override { return call("resolve"); }
  json initialize(const json&, const json&) override { return call("initialize"); }
  json warmup(const json&, const json&) override { return call("warmup"); }
  json snapshot(const json&, const json&) override { return call("snapshot"); }
  json flush(const json&, const json&) override { return call("flush"); }
  json execute(const json& context, const json& payload) override {
    json result = call("execute");
    json outputs = json::object();
    const json specs = context.value("node", json::object()).value("data_plane", json::object()).value("outputs", json::array());
    if (specs.is_array()) {
      for (const auto& spec : specs) {
        if (!spec.is_object()) {
          continue;
        }
        const std::string port_id = jsonString(spec, "port_id");
        if (port_id.empty()) {
          continue;
        }
        outputs[port_id] = {
            {"port_id", port_id},
            {"contract_id", jsonString(spec, "contract_id")},
            {"frame_contract", jsonString(spec, "frame_contract")},
            {"value_kind", jsonString(spec, "value_kind")},
            {"source_node", jsonString(node_, "node_id")},
            {"source_operator", jsonString(node_, "operator_id")},
            {"input_summary", summarizeJson(payload)},
        };
      }
    }
    result["outputs"] = outputs;
    return result;
  }

 private:
  json call(const std::string& event) {
    counters_[event] = counters_.value(event, 0) + 1;
    return {
        {"adapter_id", jsonString(node_, "adapter_id")},
        {"operator_id", jsonString(node_, "operator_id")},
        {"adapter_protocol", protocol()},
        {"state", event},
        {"duration_ms", 0},
    };
  }

  json node_;
  json counters_ = json::object();
};

class ExternalProcessAdapterSession final : public IAdapterSession {
 public:
  ExternalProcessAdapterSession(
      json node,
      json operator_snapshot,
      json model_binding,
      json registry_entry,
      NativeWorkflowOptions options,
      fs::path trace_dir)
      : node_(std::move(node)),
        operator_snapshot_(std::move(operator_snapshot)),
        model_binding_(std::move(model_binding)),
        registry_entry_(std::move(registry_entry)),
        options_(std::move(options)),
        trace_dir_(std::move(trace_dir)) {
    if (!registry_entry_.contains("command")) {
      throw std::runtime_error("json_file adapter requires command: " + jsonString(node_, "adapter_id"));
    }
    working_directory_ = resolveTemplatePath(
        jsonString(registry_entry_, "working_directory", pathString(options_.workspace_root)),
        options_);
    timeout_ms_ = jsonInt(registry_entry_, "timeout_ms", 30000);
  }

  std::string protocol() const override {
    return jsonString(registry_entry_, "protocol", "json_file.v1");
  }

  json summary() const override {
    const bool execute_supported = jsonBool(registry_entry_, "execute_supported", false);
    const std::string capability_status =
        jsonString(registry_entry_, "capability_status", execute_supported ? "experimental" : "preflight_only");
    return {
        {"node_id", jsonString(node_, "node_id")},
        {"operator_id", jsonString(node_, "operator_id")},
        {"adapter_id", jsonString(node_, "adapter_id")},
        {"execution_kind", jsonString(node_, "execution_kind")},
        {"protocol", protocol()},
        {"session_contract", kAdapterSessionContract},
        {"session_mode", "external_process_per_event"},
        {"working_directory", pathString(working_directory_)},
        {"timeout_ms", timeout_ms_},
        {"counters", counters_},
    };
  }

  void shutdown() override {
    if (!shutdown_called_) {
      shutdown_called_ = true;
    }
  }

  json describe(const json& context, const json& payload) override { return call("describe", context, payload); }
  json resolve(const json& context, const json& payload) override { return call("resolve", context, payload); }
  json initialize(const json& context, const json& payload) override { return call("initialize", context, payload); }
  json warmup(const json& context, const json& payload) override { return call("warmup", context, payload); }
  json execute(const json& context, const json& payload) override { return call("execute", context, payload); }
  json snapshot(const json& context, const json& payload) override { return call("snapshot", context, payload); }
  json flush(const json& context, const json& payload) override { return call("flush", context, payload); }

 private:
  std::vector<std::string> rawCommand() const {
    std::vector<std::string> command;
    const json& raw = registry_entry_.at("command");
    if (raw.is_array()) {
      for (const auto& item : raw) {
        if (item.is_string()) {
          command.push_back(item.get<std::string>());
        }
      }
    } else if (raw.is_string()) {
      command.push_back(raw.get<std::string>());
    }
    if (command.empty()) {
      throw std::runtime_error("Adapter command is empty: " + jsonString(node_, "adapter_id"));
    }
    return command;
  }

  std::string buildCommandLine(
      const json& context,
      const fs::path& request_path,
      const fs::path& response_path) const {
    const std::vector<std::string> raw = rawCommand();
    bool has_request_placeholder = false;
    bool has_response_placeholder = false;
    for (const std::string& part : raw) {
      has_request_placeholder = has_request_placeholder || part.find("{request_json}") != std::string::npos;
      has_response_placeholder = has_response_placeholder || part.find("{response_json}") != std::string::npos;
    }

    std::vector<std::string> args;
    args.reserve(raw.size() + 2);
    for (const std::string& part : raw) {
      args.push_back(resolveAdapterCommandTemplate(part, options_, context, request_path, response_path));
    }
    if (!has_request_placeholder) {
      args.push_back(pathString(request_path));
    }
    if (!has_response_placeholder) {
      args.push_back(pathString(response_path));
    }

    if (registry_entry_.at("command").is_string()) {
      std::string line = args.front();
      for (std::size_t i = 1; i < args.size(); ++i) {
        line += " ";
        line += quoteProcessArg(args[i]);
      }
      return line;
    }

    std::string line;
    for (const std::string& arg : args) {
      if (!line.empty()) {
        line += " ";
      }
      line += quoteProcessArg(arg);
    }
    return line;
  }

  json call(const std::string& event, const json& context, const json& payload) {
    counters_[event] = counters_.value(event, 0) + 1;
    const fs::path trace_root = jsonString(context, "trace_run_dir", jsonString(context, "run_dir"));
    const fs::path call_dir = trace_root / "ac" / hashId("n", jsonString(node_, "node_id"));
    fs::create_directories(call_dir);
    const std::string base = std::to_string(jsonInt(counters_, event, 1)) + "." + stableId(event);
    const fs::path request_path = call_dir / (base + ".req.json");
    const fs::path response_path = call_dir / (base + ".res.json");
    const json request = {
        {"schema_version", "flightenv.platform.adapter_call.v1"},
        {"event", event},
        {"run_id", jsonString(context, "run_id")},
        {"workflow_id", jsonString(context, "workflow_id")},
        {"object_id", jsonString(context, "object_id")},
        {"run_dir", jsonString(context, "run_dir")},
        {"loop_iteration_index", jsonInt(context, "loop_iteration_index", 0)},
        {"node", context.value("node", node_)},
        {"operator_snapshot", operator_snapshot_},
        {"resource_locks", context.value("resource_locks", json::object())},
        {"model_binding", model_binding_},
        {"payload", payload},
    };
    writeJson(request_path, request);

    const std::string command_line = buildCommandLine(context, request_path, response_path);
    const auto start = std::chrono::steady_clock::now();
    const int exit_code = runProcessCommand(command_line, working_directory_, timeout_ms_, trace_root);
    const int duration_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    if (exit_code != 0) {
      throw std::runtime_error("External adapter command failed: adapter=" + jsonString(node_, "adapter_id") +
                               " event=" + event + " exit_code=" + std::to_string(exit_code));
    }
    if (!fs::exists(response_path)) {
      throw std::runtime_error("External adapter response not found: " + pathString(response_path));
    }
    json parsed = readJson(response_path);
    const std::string status_text = jsonString(parsed, "status", "ok");
    if (status_text != "ok") {
      throw std::runtime_error("External adapter returned status '" + status_text + "' for " +
                               jsonString(node_, "adapter_id") + "/" + event +
                               ": " + jsonString(parsed, "reason"));
    }
    json result = parsed.value("result", json::object());
    if (!result.is_object()) {
      throw std::runtime_error("External adapter result must be an object: " + jsonString(node_, "adapter_id"));
    }
    result["adapter_protocol"] = protocol();
    result["duration_ms"] = duration_ms;
    result["session_mode"] = "external_process_per_event";
    result["request_ref"] = pathString(request_path);
    result["response_ref"] = pathString(response_path);
    result["timeout_ms"] = timeout_ms_;
    return result;
  }

  json node_;
  json operator_snapshot_;
  json model_binding_;
  json registry_entry_;
  NativeWorkflowOptions options_;
  fs::path trace_dir_;
  fs::path working_directory_;
  int timeout_ms_ = 30000;
  json counters_ = json::object();
  bool shutdown_called_ = false;
};

class ManagedExternalAdapterSession final : public IAdapterSession {
 public:
  ManagedExternalAdapterSession(json node, json registry_entry)
      : node_(std::move(node)), registry_entry_(std::move(registry_entry)) {}

  std::string protocol() const override {
    return jsonString(registry_entry_, "protocol", "managed_external.v1");
  }

  json summary() const override {
    const bool execute_supported = jsonBool(registry_entry_, "execute_supported", false);
    const std::string capability_status =
        jsonString(registry_entry_, "capability_status", execute_supported ? "experimental" : "preflight_only");
    return {
        {"node_id", jsonString(node_, "node_id")},
        {"operator_id", jsonString(node_, "operator_id")},
        {"adapter_id", jsonString(node_, "adapter_id")},
        {"execution_kind", jsonString(node_, "execution_kind")},
        {"protocol", protocol()},
        {"session_contract", kAdapterSessionContract},
        {"session_mode", "declared_external_lifecycle"},
        {"backend_status", capability_status},
        {"execute_supported", execute_supported},
        {"capabilities", registry_entry_.value("capabilities", json::object())},
        {"health_check", registry_entry_.value("health_check", json::object())},
        {"restart_policy", registry_entry_.value("restart_policy", json::object())},
        {"counters", counters_},
    };
  }

  void shutdown() override { counters_["shutdown"] = counters_.value("shutdown", 0) + 1; }

  json describe(const json&, const json&) override { return lifecycleOnly("describe"); }
  json resolve(const json&, const json&) override { return lifecycleOnly("resolve"); }
  json initialize(const json&, const json&) override { return lifecycleOnly("initialize"); }
  json warmup(const json&, const json&) override { return lifecycleOnly("warmup"); }
  json snapshot(const json&, const json&) override { return lifecycleOnly("snapshot"); }
  json flush(const json&, const json&) override { return lifecycleOnly("flush"); }

  json execute(const json&, const json&) override {
    counters_["execute"] = counters_.value("execute", 0) + 1;
    if (!jsonBool(registry_entry_, "execute_supported", false)) {
      throw std::runtime_error("Adapter protocol '" + protocol() +
                               "' is registered as non-executable in this RuntimeHost build. "
                               "Set execute_supported=true only after a native session implementation, "
                               "health check, timeout, and restart policy are wired.");
    }
    throw std::runtime_error("Adapter protocol '" + protocol() +
                             "' is marked execute_supported=true, but this C++ RuntimeHost build "
                             "does not provide a native executable session for it yet.");
  }

 private:
  json lifecycleOnly(const std::string& event) {
    counters_[event] = counters_.value(event, 0) + 1;
    const bool execute_supported = jsonBool(registry_entry_, "execute_supported", false);
    const std::string capability_status =
        jsonString(registry_entry_, "capability_status", execute_supported ? "experimental" : "preflight_only");
    return {
        {"adapter_id", jsonString(node_, "adapter_id")},
        {"operator_id", jsonString(node_, "operator_id")},
        {"adapter_protocol", protocol()},
        {"state", event},
        {"duration_ms", 0},
        {"session_mode", "declared_external_lifecycle"},
        {"backend_status", capability_status},
        {"execute_supported", execute_supported},
        {"health_check", registry_entry_.value("health_check", json::object())},
        {"restart_policy", registry_entry_.value("restart_policy", json::object())},
    };
  }

  json node_;
  json registry_entry_;
  json counters_ = json::object();
};

}  // namespace

class NativeWorkflowRunner::Impl {
 public:
  explicit Impl(NativeWorkflowOptions options) : options_(std::move(options)) {
    options_.workspace_root = fs::absolute(options_.workspace_root);
    options_.pdk_root = fs::absolute(options_.pdk_root);
    options_.compiled_workflow = fs::absolute(options_.compiled_workflow);
    options_.adapter_registry = fs::absolute(options_.adapter_registry);
    (void)resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode);
    (void)resolveRuntimeTypedBufferPersistence(options_.typed_buffer_persistence);
    loadCompiledWorkflow();
    loadAdapterRegistry();
  }

  ~Impl() {
    shutdown();
  }

  NativeWorkflowResult run(const NativeWorkflowRequest& request) {
    NativeWorkflowRequest req = request;
    req.run_dir = fs::absolute(req.run_dir);
    if (req.branch_id.empty()) {
      req.branch_id = req.run_id.empty() ? std::string("default") : req.run_id;
    }
    if (req.timeline_id.empty()) {
      req.timeline_id = workflow_id_.empty() ? std::string("workflow") : workflow_id_;
    }
    fs::create_directories(req.run_dir);
    appendTrace(req.run_dir, "native_workflow_run_begin workflow=" + workflow_id_ +
                               " run_id=" + req.run_id +
                               " prepare_only=" + (req.prepare_only ? std::string("true") : std::string("false")));

    json lifecycle_events = json::array();
    json scheduler_events = json::array();
    json node_snapshots = json::array();
    json uncertainty_nodes = json::array();
    json checkpoints = json::array();
    json data_plane_entries = json::array();
    json input_artifacts = json::array();
    json output_artifacts = json::array();
    json loop_iterations = json::array();
    json runtime_packets = json::array();
    json outputs = json::object();
    RuntimeTimeScheduler time_scheduler(time_plan_);
    int failed_nodes = 0;
    int iteration_count = 0;
    flightenv::platform::ThreadSafePortStore port_store;
    RuntimePortSampleBuffer port_sample_buffer;
    RuntimeReadyQueueExecutor ready_executor =
        RuntimeReadyQueueExecutor::fromSchedulerPlan(scheduler_plan_);
    const flightenv::platform::ReadyQueueScheduler& pdk_scheduler = ready_executor.scheduler();
    const flightenv::platform::ThreadPoolExecutorDescriptor& pdk_executor = ready_executor.executor();

    json previous_seed = seedFromRuntimeOutputs(req.seed_runtime_outputs);
    const json external_observations = loadExternalObservations(req.external_observation_stream);
    const json loop_policy = workflowLoopPolicy(req.max_iterations);
    const int max_iterations = req.prepare_only ? 1 : std::max(1, jsonInt(loop_policy, "max_iterations", req.max_iterations));
    const double base_dt_s = jsonDouble(loop_policy, "base_dt_s", 0.0);
    const double output_period_s = jsonDouble(loop_policy, "output_period_s", base_dt_s);
    if (!req.prepare_only && nodes_.empty() &&
        estimation_plan_.value("summary", json::object()).value("estimation_system_count", 0) > 0) {
      appendTrace(req.run_dir, "runtime_estimation_service_begin");
      estimation::RuntimeEstimationService service;
      estimation::RuntimeEstimationRequest estimation_request;
      estimation_request.run_dir = req.run_dir;
      estimation_request.compiled_workflow_dir = options_.compiled_workflow;
      estimation_request.run_id = req.run_id;
      estimation_request.workflow_id = workflow_id_;
      estimation_request.object_id = object_id_;
      estimation_request.branch_id = req.branch_id;
      estimation_request.timeline_id = req.timeline_id;
      estimation_request.estimation_plan = estimation_plan_;
      estimation_request.scheduler_plan = scheduler_plan_;
      estimation_request.workflow_snapshot = workflow_snapshot_;
      estimation_request.external_observations = external_observations;
      estimation_request.max_frames = max_iterations;
      const estimation::RuntimeEstimationResult estimation_result = service.runSerial(estimation_request);
      appendTrace(req.run_dir, "runtime_estimation_service_end frame_count=" +
                               std::to_string(estimation_result.frame_count));
      return NativeWorkflowResult{
          estimation_result.exit_code,
          estimation_result.frame_count,
          estimation_result.failed_frame_count,
          estimation_result.summary};
    }

    if (req.prepare_only) {
      for (const auto& node : nodes_) {
        appendTrace(req.run_dir, "prepare_node_begin node=" + jsonString(node, "node_id"));
        const int unused_iteration = 0;
        const json time_info = time_scheduler.baseTimeInfo(node, unused_iteration, 0.0);
        const json node_result = prepareNode(node, req, time_info, lifecycle_events, scheduler_events, unused_iteration);
        node_snapshots.push_back(node_result);
        appendTrace(req.run_dir, "prepare_node_end node=" + jsonString(node, "node_id"));
      }
      writeArtifacts(req, lifecycle_events, scheduler_events, node_snapshots, uncertainty_nodes, checkpoints,
                     data_plane_entries, input_artifacts, output_artifacts, loop_iterations, runtime_packets, outputs,
                     previous_seed, external_observations, port_store, pdk_scheduler, pdk_executor,
                     ready_executor,
                     0, failed_nodes, true);
      return NativeWorkflowResult{0, 0, 0, {{"status", "prepared"}, {"session_count", sessions_.size()}}};
    }

    RuntimeEventLoop event_loop;
    time_scheduler.seedWorkflowEvents(event_loop.queue(), nodes_, max_iterations, base_dt_s, output_period_s);
    if (external_observations.is_array()) {
      const int input_event_count =
          std::min<int>(max_iterations, static_cast<int>(external_observations.size()));
      for (int input_index = 0; input_index < input_event_count; ++input_index) {
        const json raw_frame = external_observations.at(static_cast<std::size_t>(input_index));
        const json frame = externalObservationFramePayload(raw_frame);
        const double sample_time_s = jsonDouble(frame, "sample_time_s", jsonDouble(frame, "time_s", input_index));
        event_loop.queue().push(RuntimeEventQueue::inputArrivedEvent(
            "external_observation",
            sample_time_s,
            input_index,
            {
                {"source", "external_observation_stream"},
                {"source_stream_path", pathString(req.external_observation_stream)},
                {"frame_index", input_index},
                {"sample_time_s", sample_time_s},
                {"has_payload", frame.is_object() && !frame.empty()},
                {"frame", frame},
            }));
      }
    }
    std::map<std::string, json> node_by_id;
    for (const auto& node : nodes_) {
      const std::string node_id = jsonString(node, "node_id");
      if (!node_id.empty()) {
        node_by_id[node_id] = node;
      }
    }
    json public_tick_outputs = json::object();
    json public_tick_effective_delta_t_by_node = json::object();
    json stop_status_by_iteration = json::object();
    int public_tick_failed_nodes = 0;
    int dispatch_tick_index = 0;
    bool stop_requested = false;
    while (!event_loop.empty()) {
      const RuntimeEvent loop_event = event_loop.next();
      if (loop_event.event_kind == "input_arrived") {
        scheduler_events.push_back(RuntimeEventLoop::schedulerEvent(
            loop_event,
            "input_arrived",
            recordExternalObservationSample(port_sample_buffer, loop_event, req.external_observation_stream)));
        continue;
      }

      if (loop_event.event_kind == "checkpoint_due") {
        scheduler_events.push_back(RuntimeEventLoop::schedulerEvent(loop_event));
        continue;
      }

      if (loop_event.event_kind == "stop_check_due") {
        const int iteration = std::max(0, loop_event.iteration_index);
        const std::string iteration_key = std::to_string(iteration);
        const json stop_status =
            stop_status_by_iteration.value(iteration_key, json::object({{"stop", false},
                                                                        {"stop_reason", "public_tick_not_finished"}}));
        scheduler_events.push_back(RuntimeEventLoop::schedulerEvent(
            loop_event,
            "stop_check_due",
            {
                {"loop_iteration_index", iteration},
                {"status", stop_status.value("stop", false) ? "stop_requested" : "continue"},
                {"stop", stop_status.value("stop", false)},
                {"stop_reason", stop_status.value("stop_reason", "")},
                {"stop_status", stop_status},
            }));
        if (stop_status.value("stop", false)) {
          stop_requested = true;
          break;
        }
        continue;
      }

      if (loop_event.event_kind == "branch_triggered") {
        scheduler_events.push_back(RuntimeEventLoop::schedulerEvent(loop_event));
        continue;
      }

      if (loop_event.event_kind == "node_due") {
        const int iteration = std::max(0, loop_event.iteration_index);
        const auto node_it = node_by_id.find(loop_event.target_id);
        if (node_it == node_by_id.end()) {
          scheduler_events.push_back(RuntimeEventLoop::schedulerEvent(
              loop_event,
              "node_due_missing_target",
              {{"loop_iteration_index", iteration}}));
          continue;
        }
        const json& node = node_it->second;
        const std::string node_id = jsonString(node, "node_id");
        appendTrace(req.run_dir, "execute_node_begin iteration=" + std::to_string(iteration) +
                                   " event_time=" + std::to_string(loop_event.event_time_s) +
                                   " node=" + node_id);
        const json base_upstream = previous_seed.is_object() ? previous_seed : json::object();
        const json target_data_plane = nodePlanInfo(data_plane_plan_, node_id);
        RuntimePortBindingResolveRequest binding_request;
        binding_request.node = node;
        binding_request.base_upstream = base_upstream;
        binding_request.external_seed =
            externalObservationSeed(external_observations, iteration, req.external_observation_stream);
        binding_request.current_outputs = outputs;
        binding_request.data_plane_info = target_data_plane;
        binding_request.port_store = &port_store;
        binding_request.branch_id = req.branch_id;
        binding_request.timeline_id = req.timeline_id;
        binding_request.event_time_ns = loop_event.event_time.nanoseconds;
        binding_request.allow_implicit_contract_port_binding =
            envFlagEnabled("FLIGHTENV_ALLOW_IMPLICIT_CONTRACT_PORT_BINDING");
        binding_request.trace = [&](const std::string& message) {
          appendTrace(req.run_dir, message);
        };
        const RuntimePortBindingResolveResult port_binding =
            RuntimePortBindingResolver::resolve(binding_request);
        json input_binding_event = RuntimeEventLoop::schedulerEvent(
            loop_event,
            "input_binding",
            port_binding.evidence);
        input_binding_event["node_id"] = node_id;
        input_binding_event["input_binding"] = port_binding.evidence;
        scheduler_events.push_back(input_binding_event);
        json upstream = port_binding.upstream;

        const RuntimeNodeDispatch cadence =
            time_scheduler.planEventDispatch(node, loop_event, base_dt_s, output_period_s);
        const json time_info = cadence.time_info;
        const RuntimeInputAlignmentResult input_alignment =
            RuntimeInputAlignment::alignNodeInputs(node, time_info, port_sample_buffer);
        RuntimeInputAlignment::applyAlignedInputs(input_alignment, upstream);
        public_tick_effective_delta_t_by_node[node_id] = cadence.effective_delta_t_s;
        if (input_alignment.hasUnavailableRequiredInputs()) {
          scheduler_events.push_back({
              {"timestamp_utc", nowUtcIso()},
              {"node_id", node_id},
              {"event", "input_alignment_blocked"},
              {"runtime_event_id", loop_event.event_id},
              {"runtime_event_kind", loop_event.event_kind},
              {"status", "blocked"},
              {"reason", "required_rate_transition_input_unavailable"},
              {"dispatch_tick_index", dispatch_tick_index},
              {"runtime_event_time_s", loop_event.event_time_s},
              {"runtime_event_time_ns", loop_event.event_time.nanoseconds},
              {"loop_iteration_index", iteration},
              {"input_binding", port_binding.evidence},
              {"input_alignment", input_alignment.evidence()},
              {"unavailable_required_inputs", input_alignment.unavailableRequiredEvidence()},
          });
          appendTrace(req.run_dir, "execute_node_deferred_by_input_alignment iteration=" +
                                     std::to_string(iteration) + " node=" + node_id);
          continue;
        }
        RuntimeReadyAdmission admission =
            ready_executor.admitNode(node, loop_event, outputs, port_store, req.branch_id, req.timeline_id);
        scheduler_events.push_back({
            {"timestamp_utc", nowUtcIso()},
            {"node_id", node_id},
            {"event", "ready_queue_admission"},
            {"runtime_event_id", loop_event.event_id},
            {"runtime_event_kind", loop_event.event_kind},
            {"status", admission.status},
            {"accepted", admission.accepted},
            {"reason", admission.reason},
            {"dispatch_tick_index", dispatch_tick_index},
            {"runtime_event_time_s", loop_event.event_time_s},
            {"runtime_event_time_ns", loop_event.event_time.nanoseconds},
            {"loop_iteration_index", iteration},
            {"input_binding", port_binding.evidence},
            {"dependency_check", admission.evidence.value("dependency_check", json::object())},
            {"port_check", admission.evidence.value("port_check", json::object())},
            {"resource_check", admission.evidence.value("resource_check", json::object())},
            {"parallel_check", admission.evidence.value("parallel_check", json::object())},
            {"deadline_check", admission.evidence.value("deadline_check", json::object())},
            {"ready_queue", admission.evidence},
        });
        if (!admission.accepted) {
          appendTrace(req.run_dir, "execute_node_deferred iteration=" + std::to_string(iteration) +
                                     " node=" + node_id + " reason=" + admission.reason);
          continue;
        }
        const std::int64_t execution_started_steady_ns = steadyNowNs();
        scheduler_events.push_back({
            {"timestamp_utc", nowUtcIso()},
            {"node_id", node_id},
            {"event", "start"},
            {"runtime_event_id", loop_event.event_id},
            {"runtime_event_kind", loop_event.event_kind},
            {"status", "running"},
            {"dispatch_tick_index", dispatch_tick_index},
            {"dispatch_time_s", cadence.public_output_time_s},
            {"dispatch_time_ns", cadence.public_output_time.nanoseconds},
            {"runtime_event_time_s", loop_event.event_time_s},
            {"runtime_event_time_ns", loop_event.event_time.nanoseconds},
            {"loop_iteration_index", iteration},
            {"execution_started_steady_ns", execution_started_steady_ns},
            {"output_period_s", cadence.output_period_s},
            {"output_period_ns", cadence.output_period.nanoseconds},
            {"effective_delta_t_s", cadence.effective_delta_t_s},
            {"effective_delta_t_ns", cadence.effective_delta_t.nanoseconds},
            {"next_due_time_s", cadence.next_due_time_s},
            {"next_due_time_ns", cadence.next_due_time.nanoseconds},
            {"input_binding", port_binding.evidence},
            {"input_alignment", input_alignment.evidence()},
            {"ready_queue", admission.evidence},
            {"scheduling_level", jsonInt(schedulerInfo(scheduler_plan_, node_id), "scheduling_level", 0)},
            {"parallel_group_id", jsonString(schedulerInfo(scheduler_plan_, node_id), "parallel_group_id")},
        });

        try {
          const json node_result = executeNode(node, upstream, req, time_info, lifecycle_events, data_plane_entries,
                                               input_artifacts, output_artifacts, port_store, iteration);
          appendTrace(req.run_dir, "execute_node_end iteration=" + std::to_string(iteration) +
                                     " event_time=" + std::to_string(loop_event.event_time_s) +
                                     " node=" + node_id);
          public_tick_outputs[node_id] = node_result.at("execute_result");
          outputs[node_id] = node_result.at("execute_result");
          time_scheduler.markExecuted(node_id, iteration, cadence, node_result.at("execute_result"));
          port_sample_buffer.recordNodeOutput(node_id, node_result.at("execute_result"), time_info, iteration);
          node_snapshots.push_back(node_result.at("node_snapshot"));
          uncertainty_nodes.push_back(node_result.at("uncertainty_node"));
          checkpoints.push_back(node_result.at("checkpoint"));
          runtime_packets.push_back(node_result.at("runtime_packet"));
          if (node_result.contains("runtime_port_packets") && node_result.at("runtime_port_packets").is_array()) {
            for (const auto& packet : node_result.at("runtime_port_packets")) {
              runtime_packets.push_back(packet);
            }
          }
          const std::int64_t execution_finished_steady_ns = steadyNowNs();
          scheduler_events.push_back({
              {"timestamp_utc", nowUtcIso()},
              {"node_id", node_id},
              {"event", "finish"},
              {"runtime_event_id", loop_event.event_id},
              {"runtime_event_kind", loop_event.event_kind},
              {"status", "ok"},
              {"duration_ms", jsonInt(node_result, "duration_ms", 0)},
              {"dispatch_tick_index", dispatch_tick_index},
              {"runtime_event_time_s", loop_event.event_time_s},
              {"runtime_event_time_ns", loop_event.event_time.nanoseconds},
              {"loop_iteration_index", iteration},
              {"execution_started_steady_ns", execution_started_steady_ns},
              {"execution_finished_steady_ns", execution_finished_steady_ns},
              {"execution_elapsed_ms",
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::nanoseconds(execution_finished_steady_ns - execution_started_steady_ns))
                   .count()},
              {"output_period_s", cadence.output_period_s},
              {"output_period_ns", cadence.output_period.nanoseconds},
              {"effective_delta_t_s", cadence.effective_delta_t_s},
              {"effective_delta_t_ns", cadence.effective_delta_t.nanoseconds},
          });
          ready_executor.completeNode(admission);
        } catch (const std::exception& exc) {
          const std::int64_t execution_finished_steady_ns = steadyNowNs();
          ready_executor.completeNode(admission);
          appendTrace(req.run_dir, "execute_node_failed iteration=" + std::to_string(iteration) +
                                     " node=" + node_id + " reason=" + exc.what());
          ++public_tick_failed_nodes;
          ++failed_nodes;
          node_snapshots.push_back({
              {"node_id", node_id},
              {"operator_id", jsonString(node, "operator_id")},
              {"adapter_id", jsonString(node, "adapter_id")},
              {"execution_kind", jsonString(node, "execution_kind")},
              {"status", "failed"},
              {"reason", exc.what()},
              {"loop_iteration_index", iteration},
          });
          scheduler_events.push_back({
              {"timestamp_utc", nowUtcIso()},
              {"node_id", node_id},
              {"event", "finish"},
              {"runtime_event_id", loop_event.event_id},
              {"runtime_event_kind", loop_event.event_kind},
              {"status", "failed"},
              {"reason", exc.what()},
              {"runtime_event_time_s", loop_event.event_time_s},
              {"runtime_event_time_ns", loop_event.event_time.nanoseconds},
              {"loop_iteration_index", iteration},
              {"execution_started_steady_ns", execution_started_steady_ns},
              {"execution_finished_steady_ns", execution_finished_steady_ns},
              {"execution_elapsed_ms",
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::nanoseconds(execution_finished_steady_ns - execution_started_steady_ns))
                   .count()},
          });
          stop_requested = true;
          break;
        }
        ++dispatch_tick_index;
        continue;
      }

      if (loop_event.event_kind != "public_tick") {
        scheduler_events.push_back(RuntimeEventLoop::schedulerEvent(loop_event, "unsupported_runtime_event"));
        continue;
      }

      const RuntimeLoopTick tick = loop_event.loop_tick;
      const int iteration = tick.iteration_index;
      const RuntimePublicFrameResult public_frame = RuntimePublicFrameBuilder::build({
          &loop_event,
          &tick,
          &nodes_,
          &data_plane_plan_,
          &port_store,
          &time_scheduler,
          &outputs,
          &public_tick_outputs,
          public_tick_effective_delta_t_by_node,
          loop_policy,
          externalObservationSeed(external_observations, iteration, req.external_observation_stream)
              .value("__external_observation__", json::object()),
          nowUtcIso(),
          base_dt_s,
          output_period_s,
          public_tick_failed_nodes,
          dispatch_tick_index,
      });
      for (const auto& event : public_frame.scheduler_events) {
        scheduler_events.push_back(event);
      }
      ++iteration_count;
      json stop_status = public_frame.stop_status;
      stop_status_by_iteration[std::to_string(iteration)] = stop_status;
      loop_iterations.push_back(stop_status);
      dispatch_tick_index = public_frame.next_dispatch_tick_index;
      if (public_frame.failed) {
        stop_requested = true;
        break;
      }
      previous_seed = recurrentSeedFromOutputs(outputs, iteration);
      public_tick_outputs = json::object();
      public_tick_effective_delta_t_by_node = json::object();
      public_tick_failed_nodes = 0;
      dispatch_tick_index = 0;
    }
    if (stop_requested) {
      appendTrace(req.run_dir, "runtime_event_loop_stopped");
    }

    writeArtifacts(req, lifecycle_events, scheduler_events, node_snapshots, uncertainty_nodes, checkpoints,
                   data_plane_entries, input_artifacts, output_artifacts, loop_iterations, runtime_packets, outputs,
                   previous_seed, external_observations, port_store, pdk_scheduler, pdk_executor,
                   ready_executor,
                   iteration_count, failed_nodes, false);
    return NativeWorkflowResult{
        failed_nodes == 0 ? 0 : 2,
        iteration_count,
        failed_nodes,
        {{"status", failed_nodes == 0 ? "ok" : "failed"}, {"run_dir", pathString(req.run_dir)}}};
  }

  json sessionSummary() const {
    json sessions = json::array();
    for (const auto& item : sessions_) {
      sessions.push_back(item.second->summary());
    }
    return {
        {"schema_version", "flightenv.platform.native_adapter_session_summary.v1"},
        {"generated_at_utc", nowUtcIso()},
        {"workflow_id", workflow_id_},
        {"object_id", object_id_},
        {"execution_backend", "native_adapter_sessions"},
        {"runtime_zero_copy_mode", runtimeZeroCopyModeName(
                                       resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode))},
        {"typed_buffer_persistence", runtimeTypedBufferPersistenceName(
                                        resolveRuntimeTypedBufferPersistence(
                                            options_.typed_buffer_persistence))},
        {"adapter_session_contract", kAdapterSessionContract},
        {"session_count", sessions.size()},
        {"sessions", sessions},
    };
  }

  void shutdown() {
    for (auto& item : sessions_) {
      item.second->shutdown();
    }
    sessions_.clear();
  }

 private:
  json adapterBackendCapabilityReport() const {
    json adapters = json::array();
    std::map<std::string, int> status_counts;
    int execute_supported_count = 0;
    int production_count = 0;
    const json registry_adapters = adapter_registry_.value("adapters", json::array());
    if (registry_adapters.is_array()) {
      for (const auto& adapter : registry_adapters) {
        if (!adapter.is_object()) {
          continue;
        }
        const std::string protocol = jsonString(adapter, "protocol", "json_file.v1");
        const bool default_execute_supported =
            protocol == "json_file.v1" || protocol == "python_worker.v1" || protocol == "dll_abi.v1";
        const bool execute_supported = jsonBool(adapter, "execute_supported", default_execute_supported);
        const std::string status = jsonString(
            adapter,
            "capability_status",
            default_execute_supported ? "production" : "preflight_only");
        status_counts[status] += 1;
        execute_supported_count += execute_supported ? 1 : 0;
        production_count += status == "production" ? 1 : 0;
        adapters.push_back({
            {"adapter_id", jsonString(adapter, "adapter_id")},
            {"execution_kind", jsonString(adapter, "execution_kind")},
            {"protocol", protocol},
            {"capability_status", status},
            {"execute_supported", execute_supported},
            {"thread_safety", jsonString(adapter, "thread_safety")},
            {"timeout_ms", jsonInt(adapter, "timeout_ms", 0)},
            {"capabilities", adapter.value("capabilities", json::object())},
            {"health_check", adapter.value("health_check", json::object())},
            {"restart_policy", adapter.value("restart_policy", json::object())},
        });
      }
    }
    return {
        {"schema_version", "flightenv.platform.adapter_backend_capability_report.v1"},
        {"generated_at_utc", nowUtcIso()},
        {"workflow_id", workflow_id_},
        {"object_id", object_id_},
        {"runtime_zero_copy_mode", runtimeZeroCopyModeName(
                                       resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode))},
        {"typed_buffer_persistence", runtimeTypedBufferPersistenceName(
                                        resolveRuntimeTypedBufferPersistence(
                                            options_.typed_buffer_persistence))},
        {"summary",
         {{"adapter_count", adapters.size()},
          {"production_count", production_count},
          {"execute_supported_count", execute_supported_count},
          {"capability_status_counts", status_counts}}},
        {"adapters", adapters},
    };
  }

  void loadCompiledWorkflow() {
    if (!fs::exists(options_.compiled_workflow)) {
      throw std::runtime_error("Compiled workflow not found: " + pathString(options_.compiled_workflow));
    }
    execution_plan_ = readJson(options_.compiled_workflow / "execution_plan.json");
    time_plan_ = readJsonIfExists(options_.compiled_workflow / "time_plan.json");
    scheduler_plan_ = readJsonIfExists(options_.compiled_workflow / "scheduler_plan.json");
    uncertainty_plan_ = readJsonIfExists(options_.compiled_workflow / "uncertainty_plan.json");
    state_store_plan_ = readJsonIfExists(options_.compiled_workflow / "state_store_plan.json");
    data_plane_plan_ = readJsonIfExists(options_.compiled_workflow / "data_plane_plan.json");
    estimation_plan_ = readJsonIfExists(options_.compiled_workflow / "estimation_plan.json");
    edge_binding_plan_ = readJson(options_.compiled_workflow / "edge_binding_plan.json");
    const fs::path rate_transition_plan_path = options_.compiled_workflow / "rate_transition_plan.json";
    const bool has_rate_transition_plan = fs::exists(rate_transition_plan_path);
    rate_transition_plan_ = has_rate_transition_plan
                                ? readJson(rate_transition_plan_path)
                                : json{
                                      {"schema_version", "flightenv.platform.rate_transition_plan.v1"},
                                      {"transitions", json::array()},
                                      {"summary",
                                       {{"transition_count", 0},
                                        {"same_rate_transition_count", 0},
                                        {"cross_rate_transition_count", 0},
                                        {"fast_to_slow_count", 0},
                                        {"slow_to_fast_count", 0},
                                        {"runtime_transition_count", 0}}},
                                  };
    workflow_snapshot_ = readJsonIfExists(options_.compiled_workflow / "workflow_snapshot.json");
    operator_snapshot_ = readJsonIfExists(options_.compiled_workflow / "operator_snapshot.json");
    resource_lock_ = readJsonIfExists(options_.compiled_workflow / "resource_lock.json");
    model_snapshot_ = readJsonIfExists(options_.compiled_workflow / "model_snapshot.json");
    json execution_nodes = requireArray(execution_plan_, "nodes", options_.compiled_workflow / "execution_plan.json");
    edge_bindings_by_target_ = json::object();
    rate_transitions_by_binding_ = json::object();
    rate_transitions_by_target_ = json::object();
    const json compiled_transitions = rate_transition_plan_.value("transitions", json::array());
    if (compiled_transitions.is_array()) {
      for (const auto& transition : compiled_transitions) {
        if (!transition.is_object()) {
          continue;
        }
        const std::string transition_id = jsonString(transition, "transition_id");
        const std::string binding_id = jsonString(transition, "binding_id");
        const std::string target_node_id = jsonString(transition, "target_node_id");
        const std::string source_node_id = jsonString(transition, "source_node_id");
        const std::string source_port_id = jsonString(transition, "source_port_id");
        const std::string target_port_id = jsonString(transition, "target_port_id");
        if (transition_id.empty() || binding_id.empty() || target_node_id.empty() ||
            source_node_id.empty() || source_port_id.empty() || target_port_id.empty()) {
          throw std::runtime_error("rate_transition_plan.json contains an incomplete transition");
        }
        if (rate_transitions_by_binding_.contains(binding_id)) {
          throw std::runtime_error(
              "rate_transition_plan.json contains duplicate transition for binding: " + binding_id);
        }
        rate_transitions_by_binding_[binding_id] = transition;
        rate_transitions_by_target_[target_node_id].push_back(transition);
      }
    }
    const json compiled_bindings = edge_binding_plan_.value("bindings", json::array());
    if (compiled_bindings.is_array()) {
      for (const auto& binding : compiled_bindings) {
        if (!binding.is_object()) {
          continue;
        }
        const std::string target_node_id = jsonString(binding, "target_node_id");
        const std::string source_node_id = jsonString(binding, "source_node_id");
        const std::string source_port_id = jsonString(binding, "source_port_id");
        const std::string target_port_id = jsonString(binding, "target_port_id");
        if (target_node_id.empty() || source_node_id.empty() ||
            source_port_id.empty() || target_port_id.empty()) {
          throw std::runtime_error("edge_binding_plan.json contains an incomplete binding");
        }
        json enriched_binding = binding;
        const std::string binding_id = jsonString(enriched_binding, "binding_id");
        if (!binding_id.empty() && rate_transitions_by_binding_.contains(binding_id)) {
          enriched_binding["rate_transition"] = rate_transitions_by_binding_.at(binding_id);
        } else if (has_rate_transition_plan) {
          throw std::runtime_error(
              "rate_transition_plan.json is missing a transition for edge binding: " + binding_id);
        }
        edge_bindings_by_target_[target_node_id].push_back(enriched_binding);
      }
    }
    if (edge_bindings_by_target_.empty() &&
        envFlagEnabled("FLIGHTENV_ALLOW_WORKFLOW_SNAPSHOT_EDGE_BINDING_FALLBACK")) {
      const json workflow = workflow_snapshot_.value("workflow", json::object());
      const json phases = workflow.value("phases", json::array());
      if (phases.is_array()) {
        for (const auto& phase : phases) {
          if (!phase.is_object()) {
            continue;
          }
          const std::string phase_id = jsonString(phase, "phase_id");
          const json stages = phase.value("stages", json::array());
          if (!stages.is_array()) {
            continue;
          }
          for (const auto& stage : stages) {
            if (!stage.is_object()) {
              continue;
            }
            const std::string stage_id = jsonString(stage, "stage_id");
            const json subgraph = stage.value("subgraph", json::object());
            const json edges = subgraph.value("edges", json::array());
            if (!edges.is_array()) {
              continue;
            }
            for (const auto& edge : edges) {
              if (!edge.is_object()) {
                continue;
              }
              const json from = edge.value("from", json::object());
              const json to = edge.value("to", json::object());
              const std::string from_node = jsonString(from, "node_id");
              const std::string to_node = jsonString(to, "node_id");
              if (from_node.empty() || to_node.empty()) {
                continue;
              }
              const std::string source_node_id = phase_id + "." + stage_id + "." + from_node;
              const std::string target_node_id = phase_id + "." + stage_id + "." + to_node;
              edge_bindings_by_target_[target_node_id].push_back({
                  {"source_node_id", source_node_id},
                  {"source_port_id", jsonString(from, "port_id")},
                  {"target_node_id", target_node_id},
                  {"target_port_id", jsonString(to, "port_id")},
                  {"source", "workflow_snapshot_fallback"},
              });
            }
          }
        }
      }
    }
    for (auto& node : execution_nodes) {
      if (!node.is_object()) {
        continue;
      }
      const std::string node_id = jsonString(node, "node_id");
      if (!edge_bindings_by_target_.contains(node_id)) {
        continue;
      }
      if (!node.contains("depends_on") || !node.at("depends_on").is_array()) {
        node["depends_on"] = json::array();
      }
      std::set<std::string> deps;
      for (const auto& dep : node.at("depends_on")) {
        if (dep.is_string()) {
          deps.insert(dep.get<std::string>());
        }
      }
      for (const auto& binding : edge_bindings_by_target_.at(node_id)) {
        const std::string source_node_id = jsonString(binding, "source_node_id");
        if (!source_node_id.empty() && deps.insert(source_node_id).second) {
          node["depends_on"].push_back(source_node_id);
        }
      }
      node["edge_bindings"] = edge_bindings_by_target_.at(node_id);
      if (rate_transitions_by_target_.contains(node_id)) {
        node["rate_transitions"] = rate_transitions_by_target_.at(node_id);
      }
    }
    nodes_ = topoSortNodes(execution_nodes);
    workflow_id_ = jsonString(execution_plan_, "workflow_id", "workflow");
    object_id_ = jsonString(execution_plan_, "object_id", "object");

    for (const auto& item : operator_snapshot_.value("operators", json::array())) {
      operator_by_id_[jsonString(item, "operator_id")] = item;
    }
    for (const auto& item : resource_lock_.value("resources", json::array())) {
      resource_by_id_[jsonString(item, "resource_id")] = item;
    }
    for (const auto& item : model_snapshot_.value("operator_bindings", json::array())) {
      model_binding_by_operator_[jsonString(item, "operator_id")] = item;
    }
  }

  void loadAdapterRegistry() {
    if (options_.adapter_registry.empty() || !fs::exists(options_.adapter_registry)) {
      if (options_.require_adapter_registry) {
        throw std::runtime_error("Adapter registry not found: " + pathString(options_.adapter_registry));
      }
      adapter_registry_ = json::object();
      return;
    }
    adapter_registry_ = readJson(options_.adapter_registry);
  }

  json resolveAdapterEntry(const json& node) const {
    if (!adapter_registry_.is_object() || !adapter_registry_.contains("adapters") ||
        !adapter_registry_.at("adapters").is_array()) {
      return json::object();
    }
    const std::string adapter_id = jsonString(node, "adapter_id");
    const std::string execution_kind = jsonString(node, "execution_kind");
    json wildcard = json::object();
    json by_kind = json::object();
    for (const auto& entry : adapter_registry_.at("adapters")) {
      if (!entry.is_object()) {
        continue;
      }
      const std::string id = jsonString(entry, "adapter_id");
      if (id == adapter_id) {
        return entry;
      }
      if (id == execution_kind && by_kind.empty()) {
        by_kind = entry;
      }
      if (id == "*" && options_.allow_wildcard_adapter && wildcard.empty()) {
        wildcard = entry;
      }
    }
    if (!by_kind.empty()) {
      return by_kind;
    }
    return wildcard;
  }

  IAdapterSession& sessionForNode(const json& node, const fs::path& trace_dir = {}) {
    const std::string node_id = jsonString(node, "node_id");
    auto found = sessions_.find(node_id);
    if (found != sessions_.end()) {
      return *found->second;
    }
    json registry_entry = resolveAdapterEntry(node);
    if (registry_entry.empty()) {
      if (options_.require_adapter_registry) {
        throw std::runtime_error(
            "No exact adapter registry entry for node: " + node_id +
            " adapter_id=" + jsonString(node, "adapter_id") +
            ". Wildcard adapters are disabled for native production runs; "
            "add a real adapter registry entry or run an explicit smoke with wildcard enabled.");
      }
      auto session = std::make_unique<RecordingAdapterSession>(node);
      auto* raw = session.get();
      sessions_[node_id] = std::move(session);
      return *raw;
    }
    const std::string protocol = jsonString(registry_entry, "protocol", "json_file.v1");
    if (protocol == "dll_abi.v1") {
      auto session = std::make_unique<DllAbiAdapterSession>(
          node,
          operator_by_id_.count(jsonString(node, "operator_id")) ? operator_by_id_.at(jsonString(node, "operator_id")) : json::object(),
          model_binding_by_operator_.count(jsonString(node, "operator_id")) ? model_binding_by_operator_.at(jsonString(node, "operator_id")) : json::object(),
          registry_entry,
          options_,
          trace_dir);
      auto* raw = session.get();
      sessions_[node_id] = std::move(session);
      return *raw;
    }
    if (protocol == "json_file.v1" || protocol == "python_worker.v1") {
      auto session = std::make_unique<ExternalProcessAdapterSession>(
          node,
          operator_by_id_.count(jsonString(node, "operator_id")) ? operator_by_id_.at(jsonString(node, "operator_id")) : json::object(),
          model_binding_by_operator_.count(jsonString(node, "operator_id")) ? model_binding_by_operator_.at(jsonString(node, "operator_id")) : json::object(),
          registry_entry,
          options_,
          trace_dir);
      auto* raw = session.get();
      sessions_[node_id] = std::move(session);
      return *raw;
    }
    if (protocol == "ros2_node.v1" || protocol == "onnx_runtime.v1" || protocol == "db_query.v1") {
      auto session = std::make_unique<ManagedExternalAdapterSession>(node, registry_entry);
      auto* raw = session.get();
      sessions_[node_id] = std::move(session);
      return *raw;
    }
    throw std::runtime_error("Unsupported adapter protocol for native backend: " + protocol);
  }

  json workflowLoopPolicy(int runtime_max_iterations) const {
    const json workflow = workflow_snapshot_.value("workflow", json::object());
    const json stop_policy = workflow.value("stop_policy", json::object());
    const json solver_policy = time_plan_.value("solver_policy", workflow.value("solver_policy", json::object()));
    const double base_dt = jsonDouble(solver_policy, "base_dt_s", 0.0);
    const double output_period = jsonDouble(solver_policy, "output_period_s", base_dt);
    int max_iterations = jsonInt(stop_policy, "max_iterations", jsonInt(solver_policy, "max_steps", 1));
    const json alternatives = stop_policy.value("alternatives", json::array());
    if (alternatives.is_array()) {
      for (const auto& alternative : alternatives) {
        if (!alternative.is_object()) {
          continue;
        }
        const std::string kind = jsonString(alternative, "kind");
        const int guard = kind == "max_iterations"
                              ? jsonInt(alternative, "max_iterations", 0)
                              : (kind == "max_steps" ? jsonInt(alternative, "max_steps", 0) : 0);
        if (guard > 0) {
          max_iterations = max_iterations > 0 ? std::min(max_iterations, guard) : guard;
        }
      }
    }
    if (runtime_max_iterations > 0) {
      max_iterations = max_iterations > 0 ? std::min(max_iterations, runtime_max_iterations) : runtime_max_iterations;
    }
    if (max_iterations <= 0) {
      max_iterations = 1;
    }
    return {
        {"enabled", max_iterations > 1},
        {"max_iterations", max_iterations},
        {"base_dt_s", base_dt},
        {"output_period_s", output_period},
        {"stop_policy", stop_policy},
    };
  }

  json nodeContext(const json& node, const NativeWorkflowRequest& req, const json& time_info, int iteration_index) const {
    json adapter_node = node;
    const std::string node_id = jsonString(node, "node_id");
    adapter_node["runtime_time"] = time_info;
    adapter_node["data_plane"] = nodePlanInfo(data_plane_plan_, node_id);
    adapter_node["loop_iteration_index"] = iteration_index;
    return {
        {"run_id", req.run_id},
        {"workflow_id", workflow_id_},
        {"object_id", object_id_},
        {"run_dir", adapterRunDirString(req.run_dir)},
        {"trace_run_dir", pathString(req.run_dir)},
        {"loop_iteration_index", iteration_index},
        {"node", adapter_node},
        {"resource_locks", resourcePayload(resource_by_id_, node)},
    };
  }

  void logLifecycle(
      json& lifecycle_events,
      const json& node,
      const std::string& event,
      const std::string& status,
      const json& time_info,
      const json& details,
      int iteration_index) const {
    lifecycle_events.push_back({
        {"timestamp_utc", nowUtcIso()},
        {"node_id", jsonString(node, "node_id")},
        {"operator_id", jsonString(node, "operator_id")},
        {"adapter_id", jsonString(node, "adapter_id")},
        {"execution_kind", jsonString(node, "execution_kind")},
        {"event", event},
        {"status", status},
        {"details",
         {
             {"time_point", time_info.value("time_point", json::object())},
             {"output_time_point", time_info.value("output_time_point", json::object())},
             {"delta_t_s", jsonDouble(time_info, "delta_t_s", 0.0)},
             {"effective_delta_t_s", jsonDouble(time_info, "effective_delta_t_s", jsonDouble(time_info, "delta_t_s", 0.0))},
             {"output_period_s", jsonDouble(time_info, "output_period_s", 0.0)},
             {"public_output_time_s", jsonDouble(time_info, "public_output_time_s", 0.0)},
             {"loop_iteration_index", iteration_index},
             {"session_mode", details.value("session_mode", "native_in_process_persistent")},
             {"duration_ms", details.value("duration_ms", 0)},
             {"adapter_protocol", details.value("adapter_protocol", "")},
         }},
    });
  }

  json prepareNode(
      const json& node,
      const NativeWorkflowRequest& req,
      const json& time_info,
      json& lifecycle_events,
      json& scheduler_events,
      int iteration_index) {
    appendTrace(req.run_dir, "session_resolve_begin node=" + jsonString(node, "node_id") +
                               " adapter=" + jsonString(node, "adapter_id"));
    IAdapterSession& base_session = sessionForNode(node, req.run_dir);
    appendTrace(req.run_dir, "session_resolve_end node=" + jsonString(node, "node_id") +
                             " adapter=" + jsonString(node, "adapter_id") +
                             " protocol=" + base_session.protocol());
    const json context = nodeContext(node, req, time_info, iteration_index);
    bool already_prepared = false;
    if (auto* dll_session = dynamic_cast<DllAbiAdapterSession*>(&base_session)) {
      already_prepared = dll_session->prepared();
    }
    if (!already_prepared) {
      for (const std::string event : {"describe", "resolve", "initialize", "warmup"}) {
        json result;
        if (event == "describe") result = base_session.describe(context, json::object());
        if (event == "resolve") result = base_session.resolve(context, {{"locked_resource_ids", node.value("resource_refs", json::array())}});
        if (event == "initialize") result = base_session.initialize(context, json::object());
        if (event == "warmup") result = base_session.warmup(context, json::object());
        logLifecycle(lifecycle_events, node, event, "ok", time_info, result, iteration_index);
      }
      if (auto* dll_session = dynamic_cast<DllAbiAdapterSession*>(&base_session)) {
        dll_session->setPrepared(true);
      }
    } else {
      logLifecycle(lifecycle_events, node, "reuse_session", "ok", time_info,
                   {{"adapter_protocol", base_session.protocol()}, {"session_mode", "native_in_process_persistent"}, {"duration_ms", 0}},
                   iteration_index);
    }
    scheduler_events.push_back({
        {"timestamp_utc", nowUtcIso()},
        {"node_id", jsonString(node, "node_id")},
        {"event", "prepare"},
        {"status", "ok"},
        {"loop_iteration_index", iteration_index},
    });
    return {
        {"node_id", jsonString(node, "node_id")},
        {"operator_id", jsonString(node, "operator_id")},
        {"adapter_id", jsonString(node, "adapter_id")},
        {"execution_kind", jsonString(node, "execution_kind")},
        {"adapter_protocol", base_session.protocol()},
        {"status", "prepared"},
        {"loop_iteration_index", iteration_index},
        {"session_summary", base_session.summary()},
    };
  }

  json executeNode(
      const json& node,
      const json& upstream,
      const NativeWorkflowRequest& req,
      const json& time_info,
      json& lifecycle_events,
      json& data_plane_entries,
      json& input_artifacts,
      json& output_artifacts,
      flightenv::platform::ThreadSafePortStore& port_store,
      int iteration_index) {
    const auto started = std::chrono::steady_clock::now();
    prepareNode(node, req, time_info, lifecycle_events, scratch_scheduler_events_, iteration_index);
    IAdapterSession& session = sessionForNode(node);
    const json context = nodeContext(node, req, time_info, iteration_index);
    const std::string node_id = jsonString(node, "node_id");
    const std::string execution_tag = runtimeExecutionTag(time_info, iteration_index, node_id);
    const json input_summary = summarizeJson(upstream);

    const json data_plane_info = nodePlanInfo(data_plane_plan_, node_id);
    RuntimeAdapterInvokeRequest invoke_request;
    invoke_request.run_dir = req.run_dir;
    invoke_request.context = context;
    invoke_request.upstream = upstream;
    invoke_request.data_plane_info = data_plane_info;
    invoke_request.time_info = time_info;
    invoke_request.iteration_index = iteration_index;
    invoke_request.node_id = node_id;
    invoke_request.runtime_zero_copy_mode = options_.runtime_zero_copy_mode;
    invoke_request.typed_buffer_persistence = options_.typed_buffer_persistence;
    invoke_request.trace = [&](const std::string& message) {
      appendTrace(req.run_dir, message);
    };
    json execute_result = RuntimeAdapterInvoker::execute(session, invoke_request);
    logLifecycle(lifecycle_events, node, "execute", "ok", time_info, execute_result, iteration_index);
    appendTrace(req.run_dir, "execute_node_log_lifecycle_execute_end iteration=" +
                               std::to_string(iteration_index) + " node=" + node_id);
    const json output_summary = summarizeJson(execute_result);

    const json snapshot = session.snapshot(context, json::object());
    logLifecycle(lifecycle_events, node, "snapshot", "ok", time_info, snapshot, iteration_index);
    const json flush_result = session.flush(context, json::object());
    logLifecycle(lifecycle_events, node, "flush", "ok", time_info, flush_result, iteration_index);

    appendTrace(req.run_dir, "execute_node_dataplane_begin iteration=" + std::to_string(iteration_index) +
                                   " node=" + node_id);
    const json node_data_plane_entries =
        makeDataPlaneEntries(data_plane_info, upstream, execute_result, time_info, iteration_index);
    for (const auto& entry : node_data_plane_entries) {
      data_plane_entries.push_back(entry);
    }
    appendTrace(req.run_dir, "execute_node_dataplane_end iteration=" + std::to_string(iteration_index) +
                                 " node=" + node_id);

    appendTrace(req.run_dir, "execute_node_packet_build_begin iteration=" + std::to_string(iteration_index) +
                                 " node=" + node_id);
    flightenv::platform::RuntimePacket packet_candidate =
        buildRuntimePacket(
            req.run_id,
            object_id_,
            req.branch_id,
            req.timeline_id,
            node_id,
            execute_result,
            time_info,
            node_data_plane_entries);
    appendTrace(req.run_dir, "execute_node_packet_build_end iteration=" + std::to_string(iteration_index) +
                               " node=" + node_id +
                               " payload_bytes=" + std::to_string(packet_candidate.inline_payload_json.size()));
    appendTrace(req.run_dir, "execute_node_portstore_write_begin iteration=" + std::to_string(iteration_index) +
                                 " node=" + node_id);
    const flightenv::platform::RuntimePacket runtime_packet = port_store.write(packet_candidate);
    appendTrace(req.run_dir, "execute_node_portstore_write_end iteration=" + std::to_string(iteration_index) +
                               " node=" + node_id);
    appendTrace(req.run_dir, "execute_node_packet_json_begin iteration=" + std::to_string(iteration_index) +
                               " node=" + node_id);
    const json runtime_packet_json = runtimePacketToJson(runtime_packet);
    appendTrace(req.run_dir, "execute_node_packet_json_end iteration=" + std::to_string(iteration_index) +
                             " node=" + node_id);
    appendTrace(req.run_dir, "execute_node_port_packets_begin iteration=" + std::to_string(iteration_index) +
                               " node=" + node_id);
    const RuntimePortPacketWriteResult port_packet_result =
        RuntimePortPacketWriter::writeOutputPortPackets(
            port_store,
            RuntimePortPacketWriteRequest{
                req.run_id,
                object_id_,
                req.branch_id,
                req.timeline_id,
                node_id,
                execute_result,
                time_info,
                node_data_plane_entries,
            });
    appendTrace(req.run_dir, "execute_node_port_packets_end iteration=" + std::to_string(iteration_index) +
                               " node=" + node_id +
                               " count=" + std::to_string(port_packet_result.packets.size()));
    const json uncertainty_info = nodePlanInfo(uncertainty_plan_, node_id);
    const json state_store_info = nodePlanInfo(state_store_plan_, node_id);
    const RuntimeNodeEvidenceResult node_evidence = RuntimeNodeEvidenceBuilder::build({
        node_id,
        jsonString(node, "operator_id"),
        jsonString(node, "adapter_id"),
        jsonString(node, "execution_kind"),
        session.protocol(),
        execution_tag,
        iteration_index,
        time_info,
        snapshot,
        runtime_packet_json,
        port_packet_result.packets,
        uncertainty_info.value("uncertainty_contract", json::object()),
        input_summary,
        output_summary,
        jsonString(state_store_info, "checkpoint_kind", "snapshot_only"),
        jsonString(state_store_info, "replay_mode", "record_replay"),
    });
    input_artifacts.push_back(node_evidence.input_artifact);
    output_artifacts.push_back(node_evidence.output_artifact);

    const int duration_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    return {
        {"status", "ok"},
        {"node_id", node_id},
        {"duration_ms", duration_ms},
        {"execute_result", execute_result},
        {"node_snapshot", node_evidence.node_snapshot},
        {"uncertainty_node", node_evidence.uncertainty_node},
        {"checkpoint", node_evidence.checkpoint},
        {"runtime_packet", runtime_packet_json},
        {"runtime_port_packets", port_packet_result.packets},
        {"runtime_port_packet_by_port", port_packet_result.packet_by_port},
    };
  }

  json buildSensorStreamArtifact(const NativeWorkflowRequest& req, const json& observations) const {
    json frames = json::array();
    if (observations.is_array()) {
      for (std::size_t i = 0; i < observations.size(); ++i) {
        const json& frame = observations.at(i);
        frames.push_back({
            {"frame_index", static_cast<int>(i)},
            {"loop_iteration_index", static_cast<int>(i)},
            {"sample_time_s", jsonDouble(frame, "sample_time_s", jsonDouble(frame, "time_s", static_cast<double>(i)))},
            {"sensor_count", frame.value("sensor_count", 0)},
            {"source", "external_observation_stream"},
            {"selected_state", frame.value("selected_state", frame.value("state", json::object()))},
            {"frame", frame},
        });
      }
    }
    return {
        {"schema_version", "flightenv.platform.sensor_stream.v1"},
        {"run_id", req.run_id},
        {"workflow_id", workflow_id_},
        {"object_id", object_id_},
        {"generated_at_utc", nowUtcIso()},
        {"source_operator_id", "external.measurement_driver"},
        {"source_stream_path", pathString(req.external_observation_stream)},
        {"frames", frames},
        {"summary",
         {
             {"frame_count", frames.size()},
             {"input_exhausted", false},
         }},
    };
  }

  void writeArtifacts(
      const NativeWorkflowRequest& req,
      const json& lifecycle_events,
      const json& scheduler_events,
      const json& node_snapshots,
      const json& uncertainty_nodes,
      const json& checkpoints,
      const json& data_plane_entries,
      const json& input_artifacts,
      const json& output_artifacts,
      const json& loop_iterations,
      const json& runtime_packets,
      const json& outputs,
      const json& seed,
      const json& external_observations,
      const flightenv::platform::ThreadSafePortStore& port_store,
      const flightenv::platform::ReadyQueueScheduler& pdk_scheduler,
      const flightenv::platform::ThreadPoolExecutorDescriptor& pdk_executor,
      const RuntimeReadyQueueExecutor& ready_executor,
      int iteration_count,
      int failed_nodes,
      bool prepare_only) const {
    const std::string status = failed_nodes == 0 ? (prepare_only ? "prepared" : "completed") : "failed";
    const RuntimeEvidenceWriter evidence_writer(req.run_dir);
    int held_output_total = 0;
    double summary_base_dt_s = 0.0;
    double summary_output_period_s = 0.0;
    if (loop_iterations.is_array()) {
      for (const auto& item : loop_iterations) {
        if (!item.is_object()) {
          continue;
        }
        held_output_total += jsonInt(item, "held_output_count", 0);
        if (summary_base_dt_s <= 0.0) {
          summary_base_dt_s = jsonDouble(item, "base_dt_s", 0.0);
        }
        if (summary_output_period_s <= 0.0) {
          summary_output_period_s = jsonDouble(item, "output_period_s", 0.0);
        }
      }
    }
    const json port_store_packets = runtimePacketArrayFromPortStore(port_store);
    const json backend_capability_report = adapterBackendCapabilityReport();
    const json typed_buffer_store_summary = RuntimeTypedBufferStore::instance().summary();
    const json pdk_runtime_core = {
        {"runtime_packet", "FlightEnvPlatform::RuntimePacket"},
        {"port_store", portStoreEvidence(port_store)},
        {"ready_queue_scheduler", readyQueueSchedulerEvidence(pdk_scheduler)},
        {"thread_pool_executor", threadPoolExecutorEvidence(pdk_executor)},
        {"ready_queue_executor", ready_executor.runtimeCoreEvidence()},
        {"adapter_session_contract", kAdapterSessionContract},
        {"legacy_json_packet_buffer_count", runtime_packets.size()},
        {"typed_packet_fields",
         {"contract_id",
          "typed_schema_id",
          "typed_dto_name",
          "typed_payload_ref",
          "typed_buffer_ref",
          "buffer_layout_id",
          "buffer_bytes",
          "zero_copy_eligible"}},
        {"zero_copy_mode", runtimeZeroCopyModeName(
                               resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode))},
        {"typed_buffer_persistence", runtimeTypedBufferPersistenceName(
                                        resolveRuntimeTypedBufferPersistence(
                                            options_.typed_buffer_persistence))},
        {"single_kernel_stage", "R2.runtime_packet_typed_ref_bridge"},
        {"remaining_host_owned_services",
         {"online_loop_clock", "branch_manager", "runtime_index_writer", "health_ledger_writer"}},
    };
    evidence_writer.writeJson(
        "adapter_lifecycle_log.json",
        {{"schema_version", "flightenv.platform.adapter_lifecycle_log.v1"},
               {"run_id", req.run_id},
               {"workflow_id", workflow_id_},
               {"object_id", object_id_},
               {"generated_at_utc", nowUtcIso()},
               {"execution_backend", "native_adapter_sessions"},
               {"events", lifecycle_events}});
    evidence_writer.writeJson(
        "scheduler_timeline.json",
        {{"schema_version", "flightenv.platform.scheduler_timeline.v1"},
               {"run_id", req.run_id},
               {"workflow_id", workflow_id_},
               {"object_id", object_id_},
               {"generated_at_utc", nowUtcIso()},
               {"events", scheduler_events}});
    evidence_writer.writeJson(
        "runtime_node_snapshot.json",
        {{"schema_version", "flightenv.platform.runtime_node_snapshot.v1"},
               {"run_id", req.run_id},
               {"workflow_id", workflow_id_},
               {"object_id", object_id_},
               {"generated_at_utc", nowUtcIso()},
               {"nodes", node_snapshots}});
    evidence_writer.writeJson(
        "runtime_outputs.json",
        {{"schema_version", "flightenv.platform.runtime_outputs.v1"},
               {"run_id", req.run_id},
               {"workflow_id", workflow_id_},
               {"object_id", object_id_},
               {"generated_at_utc", nowUtcIso()},
               {"last_loop_iteration_index", std::max(0, iteration_count - 1)},
               {"typed_buffer_store", typed_buffer_store_summary},
               {"outputs", outputs}});
    evidence_writer.writeJson(
        "runtime_loop_summary.json",
        {{"schema_version", "flightenv.platform.runtime_loop_summary.v1"},
               {"run_id", req.run_id},
               {"workflow_id", workflow_id_},
               {"object_id", object_id_},
               {"generated_at_utc", nowUtcIso()},
               {"iterations", loop_iterations},
               {"summary",
                {{"iteration_count", iteration_count},
                 {"failed_nodes", failed_nodes},
                 {"base_dt_s", summary_base_dt_s},
                 {"output_period_s", summary_output_period_s},
                 {"held_output_count", held_output_total}}}});
    evidence_writer.writeJson(
        "runtime_packets.json",
        {{"schema_version", "flightenv.platform.runtime_packets.v1"},
               {"run_id", req.run_id},
               {"workflow_id", workflow_id_},
               {"object_id", object_id_},
               {"branch_id", req.branch_id},
               {"timeline_id", req.timeline_id},
               {"generated_at_utc", nowUtcIso()},
               {"producer", "ThreadSafePortStore"},
               {"summary", portStoreEvidence(port_store)},
               {"packets", port_store_packets}});
    evidence_writer.writeJson(
        "data_plane_manifest.json",
        {{"schema_version", "flightenv.platform.data_plane_manifest.v1"},
               {"run_id", req.run_id},
               {"workflow_id", workflow_id_},
               {"object_id", object_id_},
               {"generated_at_utc", nowUtcIso()},
               {"entries", data_plane_entries},
               {"summary", {{"entry_count", data_plane_entries.size()}}}});
    evidence_writer.writeJson(
        "uncertainty_evidence.json",
        {{"schema_version", "flightenv.platform.uncertainty_evidence.v1"},
               {"run_id", req.run_id},
               {"workflow_id", workflow_id_},
               {"object_id", object_id_},
               {"generated_at_utc", nowUtcIso()},
               {"nodes", uncertainty_nodes}});
    evidence_writer.writeJson(
        "state_checkpoint.json",
        {{"schema_version", "flightenv.platform.state_checkpoint.v1"},
               {"run_id", req.run_id},
               {"workflow_id", workflow_id_},
               {"object_id", object_id_},
               {"generated_at_utc", nowUtcIso()},
               {"checkpoints", checkpoints}});
    evidence_writer.writeJson(
        "runtime_artifacts.json",
        {{"schema_version", "flightenv.platform.runtime_artifacts.v1"},
               {"run_id", req.run_id},
               {"workflow_id", workflow_id_},
               {"object_id", object_id_},
               {"generated_at_utc", nowUtcIso()},
               {"inputs", input_artifacts},
               {"outputs", output_artifacts}});
    evidence_writer.writeJson("sensor_stream.json", buildSensorStreamArtifact(req, external_observations));
    evidence_writer.writeJson("adapter_session_summary.json", sessionSummary());
    evidence_writer.writeJson("adapter_backend_capability_report.json", backend_capability_report);
    evidence_writer.writeJson("edge_binding_plan.json", edge_binding_plan_);
    evidence_writer.writeJson("rate_transition_plan.json", rate_transition_plan_);
    evidence_writer.writeJson(
        "runtime_evidence.json",
        {{"schema_version", "flightenv.platform.runtime_evidence.v1"},
               {"run_id", req.run_id},
               {"workflow_id", workflow_id_},
               {"object_id", object_id_},
               {"branch_id", req.branch_id},
               {"timeline_id", req.timeline_id},
               {"status", status},
               {"generated_at_utc", nowUtcIso()},
               {"execution_backend", "native_adapter_sessions"},
               {"runtime_core", pdk_runtime_core},
               {"adapter_registry_ref", pathString(options_.adapter_registry)},
               {"compiled_workflow_dir", pathString(options_.compiled_workflow)},
               {"initial_seed", seed},
               {"summary",
                {{"node_count", nodes_.size()},
                 {"iteration_count", iteration_count},
                 {"failed_nodes", failed_nodes},
                 {"zero_copy_mode", runtimeZeroCopyModeName(
                                        resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode))},
                 {"typed_buffer_persistence", runtimeTypedBufferPersistenceName(
                                                resolveRuntimeTypedBufferPersistence(
                                                    options_.typed_buffer_persistence))},
                 {"adapter_session_count", sessions_.size()},
                 {"adapter_backend_capability", backend_capability_report.value("summary", json::object())},
                 {"typed_buffer_store", typed_buffer_store_summary},
                 {"runtime_packet_count", port_store_packets.size()},
                 {"port_store_node_output_count", portStoreEvidence(port_store).value("node_output_count", 0)},
                 {"edge_binding_count", edge_binding_plan_.value("summary", json::object()).value("binding_count", 0)},
                 {"rate_transition_count",
                  rate_transition_plan_.value("summary", json::object()).value("transition_count", 0)},
                 {"cross_rate_transition_count",
                  rate_transition_plan_.value("summary", json::object()).value("cross_rate_transition_count", 0)},
                 {"runtime_rate_transition_count",
                  rate_transition_plan_.value("summary", json::object()).value("runtime_transition_count", 0)},
                 {"ready_queue_plan_node_count", pdk_scheduler.plan_nodes.size()},
                 {"worker_pool_size", pdk_executor.options.max_workers},
                 {"pdk_workflow_process_spawned", false},
                 {"operator_process_spawned_by_host", false}}},
               {"refs",
               {{"adapter_lifecycle_log", "adapter_lifecycle_log.json"},
                 {"adapter_session_summary", "adapter_session_summary.json"},
                 {"adapter_backend_capability_report", "adapter_backend_capability_report.json"},
                 {"runtime_node_snapshot", "runtime_node_snapshot.json"},
                 {"runtime_outputs", "runtime_outputs.json"},
                 {"runtime_loop_summary", "runtime_loop_summary.json"},
                 {"edge_binding_plan", "edge_binding_plan.json"},
                 {"rate_transition_plan", "rate_transition_plan.json"},
                 {"data_plane_manifest", "data_plane_manifest.json"},
                 {"state_checkpoint", "state_checkpoint.json"},
                 {"sensor_stream", "sensor_stream.json"}}}});
  }

  NativeWorkflowOptions options_;
  json execution_plan_ = json::object();
  json time_plan_ = json::object();
  json scheduler_plan_ = json::object();
  json uncertainty_plan_ = json::object();
  json state_store_plan_ = json::object();
  json data_plane_plan_ = json::object();
  json estimation_plan_ = json::object();
  json edge_binding_plan_ = json::object();
  json rate_transition_plan_ = json::object();
  json workflow_snapshot_ = json::object();
  json operator_snapshot_ = json::object();
  json resource_lock_ = json::object();
  json model_snapshot_ = json::object();
  json adapter_registry_ = json::object();
  json edge_bindings_by_target_ = json::object();
  json rate_transitions_by_binding_ = json::object();
  json rate_transitions_by_target_ = json::object();
  std::vector<json> nodes_;
  std::map<std::string, json> operator_by_id_;
  std::map<std::string, json> resource_by_id_;
  std::map<std::string, json> model_binding_by_operator_;
  std::map<std::string, std::unique_ptr<IAdapterSession>> sessions_;
  std::string workflow_id_;
  std::string object_id_;
  json scratch_scheduler_events_ = json::array();
};

NativeWorkflowRunner::NativeWorkflowRunner(NativeWorkflowOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

NativeWorkflowRunner::~NativeWorkflowRunner() = default;

NativeWorkflowResult NativeWorkflowRunner::run(const NativeWorkflowRequest& request) {
  return impl_->run(request);
}

nlohmann::json NativeWorkflowRunner::sessionSummary() const {
  return impl_->sessionSummary();
}

void NativeWorkflowRunner::shutdown() {
  impl_->shutdown();
}

}  // namespace FlightEnvPlatformRuntime
