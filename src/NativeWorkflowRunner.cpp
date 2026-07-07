/**
 * @file NativeWorkflowRunner.cpp
 * @brief 瀹炵幇鏈湴 workflow 鎵ц鍣ㄣ€?
 *
 * 澶ф锛氳繖鏄?platform-runtime 褰撳墠鏈€鏍稿績鐨勬墽琛岃矾寰勫疄鐜般€?
 * 鍏蜂綋锛氬畠璐熻矗瑙ｆ瀽瀵硅薄鍖呭拰 workflow銆佸噯澶囩鍙ｃ€佽皟鐢?adapter/operator銆佹帹杩涙椂闂村拰鏀堕泦杈撳嚭銆?
 * 琚皝浣跨敤锛氳 PlatformRuntimeHost銆乺untime CLI銆佺鍒扮娴嬭瘯鍜?launcher/SDK 闂存帴浣跨敤銆?
 * 浣跨敤璋侊細浣跨敤 PDK 濂戠害銆丄dapterSession銆乼ime 缁勪欢銆乶lohmann::json銆佹枃浠剁郴缁熷拰鏁版嵁闈㈠紩鐢ㄣ€?
 * 鎷嗗垎鍒ゆ柇锛氳兘鎬荤粨浣嗘槑鏄惧亸澶э紱鍚庣画搴旂户缁媶 backend executor銆乸ort binding銆乵aterialization銆乪vidence writer銆?
 */

#include "FlightEnvPlatformRuntime/NativeWorkflowRunner.hpp"

#include "NativeAdapterSessions.hpp"
#include "NativeWorkflowCompiledState.hpp"
#include "NativeWorkflowDispatchLoop.hpp"
#include "NativeWorkflowEvidenceWriter.hpp"
#include "NativeWorkflowNodePreparation.hpp"
#include "NativeWorkflowNodeResultCommit.hpp"

#include "FlightEnvPlatform/Adapter/AdapterAbi.hpp"
#include "FlightEnvPlatform/Runtime/ReadyQueueScheduler.hpp"
#include "FlightEnvPlatform/Runtime/ThreadPoolExecutor.hpp"
#include "FlightEnvPlatform/Runtime/ThreadSafePortStore.hpp"
#include "FlightEnvPlatformRuntime/AdapterSession.hpp"
#include "FlightEnvPlatformRuntime/RuntimeAdapterInvoker.hpp"
#include "FlightEnvPlatformRuntime/RuntimeEventLoop.hpp"
#include "FlightEnvPlatformRuntime/RuntimeReadyQueueExecutor.hpp"
#include "FlightEnvPlatformRuntime/RuntimePublicFrameBuilder.hpp"
#include "FlightEnvPlatformRuntime/RuntimeTimeScheduler.hpp"
#include "FlightEnvPlatformRuntime/RuntimeTypedBufferStore.hpp"
#include "FlightEnvPlatformRuntime/RuntimeZeroCopyPolicy.hpp"
#include "FlightEnvPlatformRuntime/estimation/EstimatorMethodRegistry.hpp"
#include "FlightEnvPlatformRuntime/estimation/RuntimeEstimationService.hpp"
#include "FlightEnvPlatformRuntime/estimation/SampleSetStore.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeEventQueue.hpp"
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
#include <optional>
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

std::vector<double> doubleArrayFromJson(const json& values) {
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

std::vector<std::string> stringArrayFromJson(const json& values) {
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

json doubleArrayJson(const std::vector<double>& values) {
  json out = json::array();
  for (double value : values) {
    out.push_back(value);
  }
  return out;
}

json stringArrayJson(const std::vector<std::string>& values) {
  json out = json::array();
  for (const std::string& value : values) {
    out.push_back(value);
  }
  return out;
}

double traceOfDoubles(const std::vector<double>& values) {
  double trace = 0.0;
  for (double value : values) {
    trace += std::isfinite(value) ? value : 0.0;
  }
  return trace;
}

std::optional<estimation::PosteriorFrame> latestEstimatorCheckpointFromSeed(
    const fs::path& seed_runtime_outputs) {
  if (seed_runtime_outputs.empty()) {
    return std::nullopt;
  }
  const fs::path seed_path = fs::absolute(seed_runtime_outputs);
  const fs::path checkpoint_path =
      fs::is_directory(seed_path) ? seed_path / "state_checkpoint.json"
                                  : seed_path.parent_path() / "state_checkpoint.json";
  const json checkpoint_payload = readJsonIfExists(checkpoint_path);
  const json checkpoints = checkpoint_payload.value("checkpoints", json::array());
  if (!checkpoints.is_array()) {
    return std::nullopt;
  }

  std::optional<estimation::PosteriorFrame> latest;
  for (const auto& item : checkpoints) {
    if (!item.is_object()) {
      continue;
    }
    const json estimator_state = item.value("estimator_state", json::object());
    if (!estimator_state.is_object() || estimator_state.empty()) {
      continue;
    }
    estimation::PosteriorFrame frame;
    frame.frame_index = jsonInt(item, "frame_index", jsonInt(item, "loop_iteration_index", 0));
    frame.sample_time_s = jsonDouble(item, "sample_time_s", 0.0);
    frame.checkpoint_id = jsonString(item, "checkpoint_id");
    frame.commit_id = jsonString(item, "commit_id");
    frame.committed = item.value("committed", false);
    frame.value_labels = stringArrayFromJson(item.value("labels", item.value("value_labels", json::array())));
    frame.state_mean = doubleArrayFromJson(item.value("state_mean", json::array()));
    frame.covariance_diag = doubleArrayFromJson(item.value("covariance_diag", json::array()));
    frame.estimator_state = estimator_state;
    frame.diagnostics = item.value("diagnostics", json::object());
    if (frame.state_mean.empty() || frame.covariance_diag.empty()) {
      continue;
    }
    latest = std::move(frame);
  }
  return latest;
}

struct ForecastUncertaintyStep {
  json output = json::object();
  json checkpoint = json::object();
  json event = json::object();
  json scheduler_event = json::object();
};

class ForecastUncertaintyTracker {
 public:
  bool initialize(const fs::path& seed_runtime_outputs) {
    const auto seed_frame = latestEstimatorCheckpointFromSeed(seed_runtime_outputs);
    if (!seed_frame || !seed_frame->estimator_state.is_object()) {
      return false;
    }
    const std::string method_kind = jsonString(seed_frame->estimator_state, "method_kind");
    if (method_kind.empty()) {
      return false;
    }
    try {
      method_ = estimation::EstimatorMethodRegistry::create(method_kind);
    } catch (const std::exception&) {
      method_.reset();
      return false;
    }
    latest_ = *seed_frame;
    method_kind_ = method_kind;
    const std::size_t state_dim =
        std::max<std::size_t>(1, latest_.state_mean.empty() ? latest_.covariance_diag.size()
                                                            : latest_.state_mean.size());
    method_->configure(latest_.estimator_state, state_dim);
    method_->restoreState(latest_.estimator_state);
    seed_checkpoint_id_ = latest_.checkpoint_id;
    seed_covariance_trace_ = traceOfDoubles(latest_.covariance_diag);
    latest_covariance_trace_ = seed_covariance_trace_;
    enabled_ = true;
    return true;
  }

  bool enabled() const {
    return enabled_ && method_ != nullptr;
  }

  ForecastUncertaintyStep step(
      const NativeWorkflowRequest& req,
      const std::string& workflow_id,
      const std::string& object_id,
      int loop_iteration_index,
      double delta_t_s) {
    if (!enabled()) {
      return {};
    }
    if (!(delta_t_s > 0.0) || !std::isfinite(delta_t_s)) {
      delta_t_s = 1.0;
    }

    estimation::RuntimeObservationFrame observation;
    observation.frame_index = latest_.frame_index + 1;
    observation.sample_time_s = latest_.sample_time_s + delta_t_s;
    observation.arrival_time_s = observation.sample_time_s;
    observation.has_arrival_time = true;
    observation.missing_observation = true;
    observation.observation_status = "predict_only_forecast";
    observation.value_labels = latest_.value_labels;
    observation.values = latest_.state_mean;
    observation.payload = {
        {"source", "forecast_uncertainty_predict_only"},
        {"seed_checkpoint_id", seed_checkpoint_id_},
        {"loop_iteration_index", loop_iteration_index},
    };

    estimation::EstimatorStepRequest request;
    request.observation = observation;
    request.previous = &latest_;
    estimation::EstimatorStepResult result = method_->predictOnly(request);
    result.posterior.estimator_state = method_->snapshotState();
    result.posterior.committed = true;
    result.posterior.commit_id =
        req.run_id + ".forecast_uncertainty.frame_" + std::to_string(observation.frame_index);
    result.posterior.diagnostics["forecast_uncertainty"] = true;
    result.posterior.diagnostics["branch_id"] = req.branch_id;
    result.posterior.diagnostics["timeline_id"] = req.timeline_id;
    result.posterior.diagnostics["seed_checkpoint_id"] = seed_checkpoint_id_;
    result.posterior.diagnostics["loop_iteration_index"] = loop_iteration_index;

    latest_ = result.posterior;
    latest_covariance_trace_ = traceOfDoubles(latest_.covariance_diag);
    ++step_count_;
    const double growth_ratio =
        seed_covariance_trace_ > 0.0 ? latest_covariance_trace_ / seed_covariance_trace_ : 0.0;

    json diagnostics = latest_.diagnostics;
    diagnostics["covariance_trace"] = latest_covariance_trace_;
    diagnostics["seed_covariance_trace"] = seed_covariance_trace_;
    diagnostics["growth_ratio"] = growth_ratio;
    diagnostics["step_count"] = step_count_;
    diagnostics["method_kind"] = method_kind_;

    const json state_payload = {
        {"contract_id", "platform.state_vector.v1"},
        {"frame_index", latest_.frame_index},
        {"sample_time_s", latest_.sample_time_s},
        {"valid_time_s", latest_.sample_time_s},
        {"checkpoint_id", latest_.checkpoint_id},
        {"labels", stringArrayJson(latest_.value_labels)},
        {"values", doubleArrayJson(latest_.state_mean)},
        {"source", "estimator_predict_only"},
    };
    const json uncertainty_payload = {
        {"contract_id", "platform.uncertainty_summary.v1"},
        {"representation", "diagonal_covariance"},
        {"frame_index", latest_.frame_index},
        {"sample_time_s", latest_.sample_time_s},
        {"checkpoint_id", latest_.checkpoint_id},
        {"covariance_diag", doubleArrayJson(latest_.covariance_diag)},
        {"covariance_trace", latest_covariance_trace_},
        {"seed_covariance_trace", seed_covariance_trace_},
        {"growth_ratio", growth_ratio},
        {"observation_update_applied", false},
        {"predict_only", true},
        {"method_kind", method_kind_},
    };

    ForecastUncertaintyStep step;
    step.output = {
        {"status", "ok"},
        {"synthetic_node", true},
        {"node_id", "__forecast_uncertainty__"},
        {"operator_id", "platform.estimation.forecast_predict_only.synthetic.v1"},
        {"execution_kind", "platform_synthetic"},
        {"outputs",
         {
             {"state.forecast", state_payload},
             {"uncertainty.forecast", uncertainty_payload},
             {"diagnostics.forecast", diagnostics},
         }},
        {"time_info",
         {
             {"public_output_time_s", latest_.sample_time_s},
             {"loop_iteration_index", loop_iteration_index},
             {"branch_id", req.branch_id},
             {"timeline_id", req.timeline_id},
         }},
    };
    step.checkpoint = {
        {"checkpoint_id", latest_.checkpoint_id},
        {"commit_id", latest_.commit_id},
        {"checkpoint_kind", "forecast_predict_only"},
        {"committed", true},
        {"commit_barrier", "forecast_predict_only"},
        {"loop_iteration_index", loop_iteration_index},
        {"frame_index", latest_.frame_index},
        {"sample_time_s", latest_.sample_time_s},
        {"labels", stringArrayJson(latest_.value_labels)},
        {"state_mean", doubleArrayJson(latest_.state_mean)},
        {"covariance_diag", doubleArrayJson(latest_.covariance_diag)},
        {"estimator_state", latest_.estimator_state},
        {"diagnostics", diagnostics},
    };
    step.event = {
        {"event", "forecast_uncertainty_predict_only"},
        {"run_id", req.run_id},
        {"workflow_id", workflow_id},
        {"object_id", object_id},
        {"branch_id", req.branch_id},
        {"timeline_id", req.timeline_id},
        {"loop_iteration_index", loop_iteration_index},
        {"frame_index", latest_.frame_index},
        {"sample_time_s", latest_.sample_time_s},
        {"method_kind", method_kind_},
        {"checkpoint_id", latest_.checkpoint_id},
        {"seed_checkpoint_id", seed_checkpoint_id_},
        {"covariance_trace", latest_covariance_trace_},
        {"seed_covariance_trace", seed_covariance_trace_},
        {"growth_ratio", growth_ratio},
    };
    step.scheduler_event = {
        {"event", "forecast_uncertainty_predict_only"},
        {"status", "completed"},
        {"synthetic_node", true},
        {"node_id", "__forecast_uncertainty__"},
        {"operator_id", "platform.estimation.forecast_predict_only.synthetic.v1"},
        {"loop_iteration_index", loop_iteration_index},
        {"frame_index", latest_.frame_index},
        {"event_time_s", latest_.sample_time_s},
        {"branch_id", req.branch_id},
        {"timeline_id", req.timeline_id},
    };
    return step;
  }

  json summary() const {
    const double growth_ratio =
        seed_covariance_trace_ > 0.0 ? latest_covariance_trace_ / seed_covariance_trace_ : 0.0;
    return {
        {"enabled", enabled()},
        {"method_kind", method_kind_},
        {"step_count", step_count_},
        {"seed_checkpoint_id", seed_checkpoint_id_},
        {"seed_covariance_trace", seed_covariance_trace_},
        {"latest_covariance_trace", latest_covariance_trace_},
        {"growth_ratio", growth_ratio},
    };
  }

 private:
  bool enabled_ = false;
  int step_count_ = 0;
  double seed_covariance_trace_ = 0.0;
  double latest_covariance_trace_ = 0.0;
  std::string method_kind_;
  std::string seed_checkpoint_id_;
  estimation::PosteriorFrame latest_;
  std::unique_ptr<estimation::IEstimatorMethod> method_;
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
    compiled_ = loadNativeWorkflowCompiledState(options_);
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
      req.timeline_id = compiled_.workflow_id.empty() ? std::string("workflow") : compiled_.workflow_id;
    }
    fs::create_directories(req.run_dir);
    appendTrace(req.run_dir, "native_workflow_run_begin workflow=" + compiled_.workflow_id +
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
    json runtime_rate_transition_nodes = json::array();
    json outputs = json::object();
    RuntimeTimeScheduler time_scheduler(compiled_.time_plan);
    int failed_nodes = 0;
    int iteration_count = 0;
    flightenv::platform::ThreadSafePortStore port_store;
    RuntimePortSampleBuffer port_sample_buffer;
    RuntimeReadyQueueExecutor ready_executor =
        RuntimeReadyQueueExecutor::fromSchedulerPlan(compiled_.scheduler_plan);
    const flightenv::platform::ReadyQueueScheduler& pdk_scheduler = ready_executor.scheduler();
    const flightenv::platform::ThreadPoolExecutorDescriptor& pdk_executor = ready_executor.executor();

    json previous_seed = seedFromRuntimeOutputs(req.seed_runtime_outputs);
    const json external_observations = loadExternalObservations(req.external_observation_stream);
    const json loop_policy = workflowLoopPolicy(req.max_iterations);
    const int max_iterations = req.prepare_only ? 1 : std::max(1, jsonInt(loop_policy, "max_iterations", req.max_iterations));
    const double base_dt_s = jsonDouble(loop_policy, "base_dt_s", 0.0);
    const double output_period_s = jsonDouble(loop_policy, "output_period_s", base_dt_s);
    ForecastUncertaintyTracker forecast_uncertainty;
    json forecast_uncertainty_events = json::array();
    if (forecast_uncertainty.initialize(req.seed_runtime_outputs)) {
      appendTrace(req.run_dir, "forecast_uncertainty_tracker_enabled");
    }
    if (!req.prepare_only && compiled_.nodes.empty() &&
        compiled_.estimation_plan.value("summary", json::object()).value("estimation_system_count", 0) > 0) {
      appendTrace(req.run_dir, "runtime_estimation_service_begin");
      estimation::RuntimeEstimationService service;
      estimation::RuntimeEstimationRequest estimation_request;
      estimation_request.run_dir = req.run_dir;
      estimation_request.compiled_workflow_dir = options_.compiled_workflow;
      estimation_request.run_id = req.run_id;
      estimation_request.workflow_id = compiled_.workflow_id;
      estimation_request.object_id = compiled_.object_id;
      estimation_request.branch_id = req.branch_id;
      estimation_request.timeline_id = req.timeline_id;
      estimation_request.estimation_plan = compiled_.estimation_plan;
      estimation_request.scheduler_plan = compiled_.scheduler_plan;
      estimation_request.workflow_snapshot = compiled_.workflow_snapshot;
      estimation_request.external_observations = external_observations;
      if (!req.seed_runtime_outputs.empty()) {
        const fs::path seed_path = fs::absolute(req.seed_runtime_outputs);
        const fs::path candidate_checkpoint =
            fs::is_directory(seed_path) ? seed_path / "state_checkpoint.json"
                                        : seed_path.parent_path() / "state_checkpoint.json";
        estimation_request.resume_state_checkpoint = readJsonIfExists(candidate_checkpoint);
      }
      estimation_request.max_frames = max_iterations;
      const estimation::RuntimeEstimationResult estimation_result = service.runScheduled(estimation_request);
      appendTrace(req.run_dir, "runtime_estimation_service_end frame_count=" +
                               std::to_string(estimation_result.frame_count));
      return NativeWorkflowResult{
          estimation_result.exit_code,
          estimation_result.frame_count,
          estimation_result.failed_frame_count,
          estimation_result.summary};
    }

    if (req.prepare_only) {
      for (const auto& node : compiled_.nodes) {
        appendTrace(req.run_dir, "prepare_node_begin node=" + jsonString(node, "node_id"));
        const int unused_iteration = 0;
        const json time_info = time_scheduler.baseTimeInfo(node, unused_iteration, 0.0);
        const json node_result = prepareNode(node, req, time_info, lifecycle_events, scheduler_events, unused_iteration);
        node_snapshots.push_back(node_result);
        appendTrace(req.run_dir, "prepare_node_end node=" + jsonString(node, "node_id"));
      }
      writeArtifacts(req, lifecycle_events, scheduler_events, node_snapshots, uncertainty_nodes, checkpoints,
                     data_plane_entries, input_artifacts, output_artifacts, loop_iterations,
                     runtime_packets, runtime_rate_transition_nodes, outputs,
                     forecast_uncertainty_events, forecast_uncertainty.summary(),
                     previous_seed, external_observations, port_store, pdk_scheduler, pdk_executor,
                     ready_executor,
                     0, failed_nodes, true);
      return NativeWorkflowResult{0, 0, 0, {{"status", "prepared"}, {"session_count", sessions_.size()}}};
    }

    RuntimeEventLoop event_loop;
    time_scheduler.seedWorkflowEvents(
        event_loop.queue(), compiled_.nodes, max_iterations, base_dt_s, output_period_s, compiled_.scheduler_table);
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
    for (const auto& node : compiled_.nodes) {
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
        const json target_data_plane = nodePlanInfo(compiled_.data_plane_plan, node_id);
        const RuntimeNodeDispatch cadence =
            time_scheduler.planEventDispatch(node, loop_event, base_dt_s, output_period_s);
        const json time_info = cadence.time_info;
        const NativeWorkflowNodeInputPreparationResult input_preparation =
            prepareNativeWorkflowNodeInputs({
                req.run_id,
                compiled_.object_id,
                req.branch_id,
                req.timeline_id,
                node_id,
                node,
                base_upstream,
                externalObservationSeed(external_observations, iteration, req.external_observation_stream),
                outputs,
                target_data_plane,
                time_info,
                loop_event.event_time.nanoseconds,
                iteration,
                &port_store,
                &port_sample_buffer,
                envFlagEnabled("FLIGHTENV_ALLOW_IMPLICIT_CONTRACT_PORT_BINDING"),
                [&](const std::string& message) {
                  appendTrace(req.run_dir, message);
                },
        });
        const RuntimePortBindingResolveResult& port_binding = input_preparation.port_binding;
        scheduler_events.push_back(
            nativeWorkflowInputBindingSchedulerEvent(loop_event, node_id, port_binding.evidence));
        json upstream = input_preparation.upstream;
        const RuntimeRateTransitionExecutionResult& transition_execution =
            input_preparation.transition_execution;
        if (transition_execution.hasEvidence()) {
          for (const auto& event : transition_execution.events) {
            scheduler_events.push_back(event);
            runtime_rate_transition_nodes.push_back(event);
          }
          for (const auto& packet : transition_execution.packets) {
            runtime_packets.push_back(packet);
          }
        }
        const RuntimeInputAlignmentResult& input_alignment = input_preparation.input_alignment;
        public_tick_effective_delta_t_by_node[node_id] = cadence.effective_delta_t_s;
        if (input_alignment.hasUnavailableRequiredInputs()) {
          scheduler_events.push_back(nativeWorkflowInputAlignmentBlockedEvent(
              loop_event,
              node_id,
              dispatch_tick_index,
              port_binding.evidence,
              input_alignment));
          appendTrace(req.run_dir, "execute_node_deferred_by_input_alignment iteration=" +
                                     std::to_string(iteration) + " node=" + node_id);
          scheduleNativeWorkflowNodeDueRetryOrDrop(
              event_loop.queue(),
              scheduler_events,
              loop_event,
              node_id,
              "required_rate_transition_input_unavailable");
          continue;
        }
        RuntimeReadyAdmission admission =
            ready_executor.admitNode(node, loop_event, outputs, port_store, req.branch_id, req.timeline_id);
        scheduler_events.push_back(nativeWorkflowReadyQueueAdmissionEvent(
            loop_event,
            node_id,
            dispatch_tick_index,
            port_binding.evidence,
            admission));
        if (!admission.accepted) {
          appendTrace(req.run_dir, "execute_node_deferred iteration=" + std::to_string(iteration) +
                                     " node=" + node_id + " reason=" + admission.reason);
          scheduleNativeWorkflowNodeDueRetryOrDrop(
              event_loop.queue(),
              scheduler_events,
              loop_event,
              node_id,
              admission.reason);
          continue;
        }
        const std::int64_t execution_started_steady_ns = steadyNowNs();
        const json scheduler_node_info = schedulerInfo(compiled_.scheduler_plan, node_id);
        scheduler_events.push_back(nativeWorkflowNodeStartEvent(
            loop_event,
            node_id,
            dispatch_tick_index,
            cadence,
            port_binding.evidence,
            input_alignment,
            admission,
            jsonInt(scheduler_node_info, "scheduling_level", 0),
            jsonString(scheduler_node_info, "parallel_group_id"),
            execution_started_steady_ns));

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
          scheduler_events.push_back(nativeWorkflowNodeFinishOkEvent(
              loop_event,
              node_id,
              dispatch_tick_index,
              cadence,
              jsonInt(node_result, "duration_ms", 0),
              execution_started_steady_ns,
              execution_finished_steady_ns));
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
          scheduler_events.push_back(nativeWorkflowNodeFinishFailedEvent(
              loop_event,
              node_id,
              exc.what(),
              execution_started_steady_ns,
              execution_finished_steady_ns));
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
          &compiled_.nodes,
          &compiled_.data_plane_plan,
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
      if (forecast_uncertainty.enabled()) {
        const double forecast_delta_t_s = output_period_s > 0.0 ? output_period_s : base_dt_s;
        const ForecastUncertaintyStep forecast_step = forecast_uncertainty.step(
            req, compiled_.workflow_id, compiled_.object_id, iteration, forecast_delta_t_s);
        if (!forecast_step.output.empty()) {
          outputs["__forecast_uncertainty__"] = forecast_step.output;
          checkpoints.push_back(forecast_step.checkpoint);
          forecast_uncertainty_events.push_back(forecast_step.event);
          scheduler_events.push_back(forecast_step.scheduler_event);
        }
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
                   data_plane_entries, input_artifacts, output_artifacts, loop_iterations,
                   runtime_packets, runtime_rate_transition_nodes, outputs,
                   forecast_uncertainty_events, forecast_uncertainty.summary(),
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
        {"workflow_id", compiled_.workflow_id},
        {"object_id", compiled_.object_id},
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
    const json registry_adapters = compiled_.adapter_registry.value("adapters", json::array());
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
        {"workflow_id", compiled_.workflow_id},
        {"object_id", compiled_.object_id},
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

  IAdapterSession& sessionForNode(const json& node, const fs::path& trace_dir = {}) {
    const std::string node_id = jsonString(node, "node_id");
    auto found = sessions_.find(node_id);
    if (found != sessions_.end()) {
      return *found->second;
    }
    json registry_entry = compiled_.resolveAdapterEntry(node, options_.allow_wildcard_adapter);
    const std::string operator_id = jsonString(node, "operator_id");
    auto session = createNativeAdapterSession(
        node,
        compiled_.operator_by_id.count(operator_id) ? compiled_.operator_by_id.at(operator_id) : json::object(),
        compiled_.model_binding_by_operator.count(operator_id) ? compiled_.model_binding_by_operator.at(operator_id)
                                                      : json::object(),
        registry_entry,
        options_,
        trace_dir);
    auto* raw = session.get();
    sessions_[node_id] = std::move(session);
    return *raw;
  }

  json workflowLoopPolicy(int runtime_max_iterations) const {
    const json workflow = compiled_.workflow_snapshot.value("workflow", json::object());
    const json stop_policy = workflow.value("stop_policy", json::object());
    const json solver_policy = compiled_.time_plan.value("solver_policy", workflow.value("solver_policy", json::object()));
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
    adapter_node["data_plane"] = nodePlanInfo(compiled_.data_plane_plan, node_id);
    adapter_node["loop_iteration_index"] = iteration_index;
    return {
        {"run_id", req.run_id},
        {"workflow_id", compiled_.workflow_id},
        {"object_id", compiled_.object_id},
        {"run_dir", adapterRunDirString(req.run_dir)},
        {"trace_run_dir", pathString(req.run_dir)},
        {"loop_iteration_index", iteration_index},
        {"node", adapter_node},
        {"resource_locks", resourcePayload(compiled_.resource_by_id, node)},
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
    const bool already_prepared = adapterSessionPrepared(base_session);
    if (!already_prepared) {
      for (const std::string event : {"describe", "resolve", "initialize", "warmup"}) {
        json result;
        if (event == "describe") result = base_session.describe(context, json::object());
        if (event == "resolve") result = base_session.resolve(context, {{"locked_resource_ids", node.value("resource_refs", json::array())}});
        if (event == "initialize") result = base_session.initialize(context, json::object());
        if (event == "warmup") result = base_session.warmup(context, json::object());
        logLifecycle(lifecycle_events, node, event, "ok", time_info, result, iteration_index);
      }
      setAdapterSessionPrepared(base_session, true);
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

    const json data_plane_info = nodePlanInfo(compiled_.data_plane_plan, node_id);
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

    const json uncertainty_info = nodePlanInfo(compiled_.uncertainty_plan, node_id);
    const json state_store_info = nodePlanInfo(compiled_.state_store_plan, node_id);
    const NativeWorkflowNodeResultCommitResult commit_result =
        commitNativeWorkflowNodeResult({
            req.run_id,
            compiled_.object_id,
            req.branch_id,
            req.timeline_id,
            node_id,
            jsonString(node, "operator_id"),
            jsonString(node, "adapter_id"),
            jsonString(node, "execution_kind"),
            session.protocol(),
            execution_tag,
            iteration_index,
            time_info,
            data_plane_info,
            upstream,
            execute_result,
            snapshot,
            uncertainty_info,
            state_store_info,
            input_summary,
            output_summary,
            &port_store,
            [&](const std::string& message) {
              appendTrace(req.run_dir, message);
            },
        });

    for (const auto& entry : commit_result.data_plane_entries) {
      data_plane_entries.push_back(entry);
    }
    const RuntimeNodeEvidenceResult& node_evidence = commit_result.node_evidence;
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
        {"runtime_packet", commit_result.runtime_packet},
        {"runtime_port_packets", commit_result.runtime_port_packets},
        {"runtime_port_packet_by_port", commit_result.runtime_port_packet_by_port},
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
      const json& runtime_rate_transition_nodes,
      const json& outputs,
      const json& forecast_uncertainty_events,
      const json& forecast_uncertainty_summary,
      const json& seed,
      const json& external_observations,
      const flightenv::platform::ThreadSafePortStore& port_store,
      const flightenv::platform::ReadyQueueScheduler& pdk_scheduler,
      const flightenv::platform::ThreadPoolExecutorDescriptor& pdk_executor,
      const RuntimeReadyQueueExecutor& ready_executor,
      int iteration_count,
      int failed_nodes,
      bool prepare_only) const {
    const json session_summary = sessionSummary();
    const json backend_capability_report = adapterBackendCapabilityReport();
    writeNativeWorkflowEvidence({
        req.run_dir,
        options_.adapter_registry,
        options_.compiled_workflow,
        req.external_observation_stream,
        req.run_id,
        compiled_.workflow_id,
        compiled_.object_id,
        req.branch_id,
        req.timeline_id,
        runtimeZeroCopyModeName(resolveRuntimeZeroCopyMode(options_.runtime_zero_copy_mode)),
        runtimeTypedBufferPersistenceName(resolveRuntimeTypedBufferPersistence(options_.typed_buffer_persistence)),
        compiled_.nodes.size(),
        sessions_.size(),
        &lifecycle_events,
        &scheduler_events,
        &node_snapshots,
        &uncertainty_nodes,
        &checkpoints,
        &data_plane_entries,
        &input_artifacts,
        &output_artifacts,
        &loop_iterations,
        &runtime_packets,
        &runtime_rate_transition_nodes,
        &outputs,
        &forecast_uncertainty_events,
        &forecast_uncertainty_summary,
        &seed,
        &external_observations,
        &session_summary,
        &backend_capability_report,
        &compiled_.edge_binding_plan,
        &compiled_.rate_transition_plan,
        &compiled_.scheduler_table,
        &compiled_.time_plan,
        &compiled_.scheduler_plan,
        &port_store,
        &pdk_scheduler,
        &pdk_executor,
        &ready_executor,
        iteration_count,
        failed_nodes,
        prepare_only,
    });
  }
  NativeWorkflowOptions options_;
  NativeWorkflowCompiledState compiled_;
  std::map<std::string, std::unique_ptr<IAdapterSession>> sessions_;
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
