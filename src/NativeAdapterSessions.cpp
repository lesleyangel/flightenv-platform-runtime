#include "NativeAdapterSessions.hpp"

#include "FlightEnvPlatform/Adapter/AdapterAbi.hpp"
#include "FlightEnvPlatformRuntime/RuntimeTypedBufferStore.hpp"
#include "FlightEnvPlatformRuntime/RuntimeZeroCopyPolicy.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
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

struct SchedulerRuntimeEventCounts {
  int input_alignment_blocked_count = 0;
  int ready_queue_rejected_count = 0;
  int node_due_retry_scheduled_count = 0;
  int node_due_dropped_count = 0;
};

SchedulerRuntimeEventCounts countSchedulerRuntimeEvents(const json& scheduler_events) {
  SchedulerRuntimeEventCounts counts;
  if (!scheduler_events.is_array()) {
    return counts;
  }
  for (const auto& event : scheduler_events) {
    if (!event.is_object()) {
      continue;
    }
    const std::string event_name = jsonString(event, "event");
    if (event_name == "input_alignment_blocked") {
      ++counts.input_alignment_blocked_count;
    } else if (event_name == "ready_queue_admission" && !jsonBool(event, "accepted", true)) {
      ++counts.ready_queue_rejected_count;
    } else if (event_name == "node_due_retry_scheduled") {
      ++counts.node_due_retry_scheduled_count;
    } else if (event_name == "node_due_dropped") {
      ++counts.node_due_dropped_count;
    }
  }
  return counts;
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

std::unique_ptr<IAdapterSession> createNativeAdapterSession(
    const json& node,
    const json& operator_snapshot,
    const json& model_binding,
    const json& registry_entry,
    const NativeWorkflowOptions& options,
    const fs::path& trace_dir) {
  const std::string node_id = jsonString(node, "node_id");
  if (registry_entry.empty()) {
    if (options.require_adapter_registry) {
      throw std::runtime_error(
          "No exact adapter registry entry for node: " + node_id +
          " adapter_id=" + jsonString(node, "adapter_id") +
          ". Wildcard adapters are disabled for native production runs; "
          "add a real adapter registry entry or run an explicit smoke with wildcard enabled.");
    }
    return std::make_unique<RecordingAdapterSession>(node);
  }

  const std::string protocol = jsonString(registry_entry, "protocol", "json_file.v1");
  if (protocol == "dll_abi.v1") {
    return std::make_unique<DllAbiAdapterSession>(
        node,
        operator_snapshot,
        model_binding,
        registry_entry,
        options,
        trace_dir);
  }
  if (protocol == "json_file.v1" || protocol == "python_worker.v1") {
    return std::make_unique<ExternalProcessAdapterSession>(
        node,
        operator_snapshot,
        model_binding,
        registry_entry,
        options,
        trace_dir);
  }
  if (protocol == "ros2_node.v1" || protocol == "onnx_runtime.v1" || protocol == "db_query.v1") {
    return std::make_unique<ManagedExternalAdapterSession>(node, registry_entry);
  }
  throw std::runtime_error("Unsupported adapter protocol for native backend: " + protocol);
}

bool adapterSessionPrepared(const IAdapterSession& session) {
  const auto* dll_session = dynamic_cast<const DllAbiAdapterSession*>(&session);
  return dll_session != nullptr && dll_session->prepared();
}

void setAdapterSessionPrepared(IAdapterSession& session, bool value) {
  if (auto* dll_session = dynamic_cast<DllAbiAdapterSession*>(&session)) {
    dll_session->setPrepared(value);
  }
}

}  // namespace FlightEnvPlatformRuntime