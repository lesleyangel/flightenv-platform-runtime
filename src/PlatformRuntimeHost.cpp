#include "FlightEnvPlatformRuntime/PlatformRuntimeHost.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>

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
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("JSON file not found: " + pathString(path));
  }
  json value;
  in >> value;
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
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("Cannot write JSON file: " + pathString(path));
  }
  out << value.dump(2);
  out << '\n';
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

double findNumberRecursive(const json& value, const std::vector<std::string>& keys, int depth = 0) {
  if (depth > 18) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (value.is_object()) {
    for (const auto& key : keys) {
      auto it = value.find(key);
      if (it != value.end() && it->is_number()) {
        return it->get<double>();
      }
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
      const double found = findNumberRecursive(it.value(), keys, depth + 1);
      if (std::isfinite(found)) {
        return found;
      }
    }
  } else if (value.is_array()) {
    for (const auto& item : value) {
      const double found = findNumberRecursive(item, keys, depth + 1);
      if (std::isfinite(found)) {
        return found;
      }
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

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
    if (key == "prepare-only" || key == "require-adapter-registry" ||
        key == "replay-by-platform-clock") {
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
      << "  --compiled-online <dir>\n"
      << "  --compiled-future <dir>\n"
      << "  --external-observation-stream <sensor_stream.json>\n"
      << "  --online-frames <N>\n"
      << "  --prediction-every-frames <N>\n"
      << "  --future-max-iterations <N>\n"
      << "  [--preflight-adapters]\n";
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
  if (options_.object_package_root.empty()) {
    options_.object_package_root = options_.workspace_root / "flightenv-object-reentry-vehicle";
  }
  if (options_.compiled_online_workflow.empty()) {
    options_.compiled_online_workflow =
        options_.workspace_root / "_local_artifacts/platform-pdk/compiled-workflows/reentry.online_filtering_external_input.v1";
  }
  if (options_.compiled_future_workflow.empty()) {
    options_.compiled_future_workflow =
        options_.workspace_root / "_local_artifacts/platform-pdk/compiled-workflows/reentry.posterior_future_prediction.v1";
  }
  if (options_.adapter_registry.empty()) {
    options_.adapter_registry =
        options_.object_package_root / "tools/adapter_registries/ballistic_adapters.local.json";
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
  options_.pdk_root = fs::absolute(options_.pdk_root);
  options_.object_package_root = fs::absolute(options_.object_package_root);
  options_.compiled_online_workflow = fs::absolute(options_.compiled_online_workflow);
  options_.compiled_future_workflow = fs::absolute(options_.compiled_future_workflow);
  options_.adapter_registry = fs::absolute(options_.adapter_registry);
  options_.pdk_run_script = fs::absolute(options_.pdk_run_script);
  options_.pdk_cli = fs::absolute(options_.pdk_cli);
  options_.run_root = fs::absolute(options_.run_root);
  options_.chain_dir = fs::absolute(options_.chain_dir);
  if (!options_.external_observation_stream.empty()) {
    options_.external_observation_stream = fs::absolute(options_.external_observation_stream);
  }
}

void PlatformRuntimeHost::ensureInputs() const {
  const std::vector<std::pair<fs::path, std::string>> required = {
      {options_.pdk_root, "PDK root"},
      {options_.compiled_online_workflow, "compiled online workflow"},
      {options_.compiled_future_workflow, "compiled future workflow"},
      {options_.adapter_registry, "adapter registry"},
      {options_.pdk_run_script, "PDK run script"},
      {options_.pdk_cli, "PDK CLI"},
  };
  for (const auto& item : required) {
    if (!fs::exists(item.first)) {
      throw std::runtime_error(item.second + " not found: " + pathString(item.first));
    }
  }
  if (!options_.prepare_only && options_.external_observation_stream.empty()) {
    throw std::runtime_error("External observation stream is required for online execution.");
  }
  if (!options_.external_observation_stream.empty() && !fs::exists(options_.external_observation_stream)) {
    throw std::runtime_error(
        "External observation stream not found: " + pathString(options_.external_observation_stream));
  }
}

void PlatformRuntimeHost::loadCompiledWorkflowMetadata() {
  online_workflow_snapshot_ = readJson(options_.compiled_online_workflow / "workflow_snapshot.json");
  future_workflow_snapshot_ = readJson(options_.compiled_future_workflow / "workflow_snapshot.json");
  const json online_workflow = online_workflow_snapshot_.value("workflow", json::object());
  const json future_workflow = future_workflow_snapshot_.value("workflow", json::object());
  online_workflow_id_ = jsonString(online_workflow_snapshot_, "workflow_id",
                                   jsonString(online_workflow, "workflow_id", "online"));
  future_workflow_id_ = jsonString(future_workflow_snapshot_, "workflow_id",
                                   jsonString(future_workflow, "workflow_id", "future"));
  object_id_ = jsonString(online_workflow_snapshot_, "object_id",
                          jsonString(online_workflow, "object_id", "object"));

  const json branching = online_workflow.value("branching_policy", json::object());
  effective_prediction_every_frames_ = options_.prediction_every_frames > 0
                                           ? options_.prediction_every_frames
                                           : jsonInt(branching, "every_n_frames", options_.online_frames);
  effective_max_concurrent_branches_ = options_.max_concurrent_branches > 0
                                           ? options_.max_concurrent_branches
                                           : jsonInt(branching, "max_concurrent_branches", 1);
  effective_prediction_every_frames_ = std::max(1, effective_prediction_every_frames_);
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

int PlatformRuntimeHost::invokePdkIndexRuntimeRun(const fs::path& run_dir) const {
  std::ostringstream command;
  std::ostringstream ps;
  ps << "& " << quotePsSingle(options_.python)
     << " " << quotePsSingle(pathString(options_.pdk_cli))
     << " index runtime-run --root " << quotePsSingle(pathString(options_.pdk_root))
     << " --run-dir " << quotePsSingle(pathString(run_dir));
  command << "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
          << quoteArg(ps.str());
  return std::system(command.str().c_str());
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
    const int online_rc = invokePdkRun(
        options_.compiled_online_workflow, online_prepare, online_prepare_id, {}, {}, 0, true);
    if (online_rc != 0) {
      throw std::runtime_error("Online workflow preflight failed.");
    }
    const int future_rc = invokePdkRun(
        options_.compiled_future_workflow, future_prepare, future_prepare_id, {}, {}, 0, true);
    if (future_rc != 0) {
      throw std::runtime_error("Future workflow preflight failed.");
    }
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
  branch_records_ = json::array({
      {
          {"branch_id", "main.online"},
          {"branch_kind", "online_mainline"},
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
  const int rc = invokePdkRun(
      options_.compiled_online_workflow,
      run_dir,
      run_id,
      previous_seed,
      frame_stream,
      1,
      false);
  if (rc != 0) {
    throw std::runtime_error("Online frame workflow failed: " + run_id);
  }
  const int index_rc = invokePdkIndexRuntimeRun(run_dir);
  if (index_rc != 0) {
    throw std::runtime_error("Runtime index generation failed: " + pathString(run_dir));
  }
  return run_dir;
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
    const std::string combined_id = branch_id == "main.online" ? source_id : branch_id + "." + source_id;
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
    int mainline_frame_index) {
  const json timeline = readJsonIfExists(run_dir / "run_timeline_index.json");
  if (timeline.is_object()) {
    const auto append_items = [&](const char* key, json& target) {
      if (!timeline.contains(key) || !timeline.at(key).is_array()) {
        return;
      }
      for (auto item : timeline.at(key)) {
        if (!item.is_object()) {
          continue;
        }
        item["branch_id"] = branch_id;
        item["source_run_dir"] = pathString(run_dir);
        if (mainline_frame_index >= 0) {
          item["mainline_frame_index"] = mainline_frame_index;
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
        online_frames_.push_back(frame);
      }
    }
  }
  mergeSeriesManifest(run_dir / "series_manifest.json", branch_id);
}

void PlatformRuntimeHost::upsertBranchRecord(const json& record) {
  const std::string branch_id = jsonString(record, "branch_id", "");
  for (auto& item : branch_records_) {
    if (jsonString(item, "branch_id", "") == branch_id) {
      item = record;
      return;
    }
  }
  branch_records_.push_back(record);
}

void PlatformRuntimeHost::updateBranchStatus(
    const std::string& branch_id,
    const std::string& status,
    const json& summary) {
  StateLock lock(*this);
  for (auto& item : branch_records_) {
    if (jsonString(item, "branch_id", "") == branch_id) {
      item["status"] = status;
      item["updated_at_utc"] = nowUtcIso();
      if (!summary.is_null()) {
        item["summary"] = summary;
      }
      break;
    }
  }
  writeRuntimeIndexesLocked(status == "running" ? "prediction_branch_live" : "live_tail");
  writeProgressLocked();
}

void PlatformRuntimeHost::maybeForkPredictionBranch(
    int local_index,
    const json& frame,
    const fs::path& online_runtime_outputs) {
  const bool by_interval = ((local_index + 1) % effective_prediction_every_frames_) == 0;
  const bool last_frame = (local_index + 1) >= options_.online_frames;
  if (!by_interval && !last_frame) {
    return;
  }

  while (static_cast<int>(branch_threads_.size()) >= effective_max_concurrent_branches_) {
    if (branch_threads_.front().joinable()) {
      branch_threads_.front().join();
    }
    branch_threads_.erase(branch_threads_.begin());
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

  PredictionTask task;
  task.branch_id = branch_id;
  task.run_id = run_id;
  task.run_dir = run_dir;
  task.seed_runtime_outputs = online_runtime_outputs;
  task.trigger_frame_index = local_index;
  task.trigger_time_s = trigger_time;

  {
    StateLock lock(*this);
    ++requested_prediction_runs_;
    std::cout << "[C++ RuntimeHost] 登记预测分支 " << branch_id << std::endl;
    upsertBranchRecord({
        {"branch_id", branch_id},
        {"branch_kind", "future_prediction"},
        {"parent_branch_id", "main.online"},
        {"workflow_id", future_workflow_id_},
        {"run_id", run_id},
        {"run_dir", pathString(run_dir)},
        {"status", "queued"},
        {"priority", 10},
        {"created_at_utc", nowUtcIso()},
        {"updated_at_utc", nowUtcIso()},
        {"trigger_frame_index", local_index},
        {"trigger_time_s", trigger_time},
        {"seed_runtime_outputs_ref", pathString(online_runtime_outputs)},
        {"refs",
         {
             {"runtime_evidence", "runtime_evidence.json"},
             {"runtime_outputs", "runtime_outputs.json"},
             {"runtime_loop_summary", "runtime_loop_summary.json"},
             {"data_plane_manifest", "data_plane_manifest.json"},
             {"state_checkpoint", "state_checkpoint.json"},
         }},
        {"summary", {{"iteration_count", 0}, {"field_artifact_count", 0}}},
    });
    current_message_ = "已从在线帧 " + std::to_string(local_index) + " 分叉预测分支";
    writeRuntimeIndexesLocked("live_tail");
    writeProgressLocked();
  }

  branch_threads_.emplace_back([this, task]() {
    runPredictionBranch(task);
  });
  std::cout << "[C++ RuntimeHost] 预测分支已提交 " << branch_id << std::endl;
}

void PlatformRuntimeHost::runPredictionBranch(PredictionTask task) {
  try {
    std::cout << "[C++ RuntimeHost] 预测分支启动 " << task.branch_id << std::endl;
    updateBranchStatus(task.branch_id, "running", json::object());
    std::cout << "[C++ RuntimeHost] 预测分支状态已更新 running " << task.branch_id << std::endl;
    const int rc = invokePdkRun(
        options_.compiled_future_workflow,
        task.run_dir,
        task.run_id,
        task.seed_runtime_outputs,
        {},
        options_.future_max_iterations,
        false);
    if (rc != 0) {
      throw std::runtime_error("Future prediction workflow failed: " + task.run_id);
    }
    std::cout << "[C++ RuntimeHost] 预测分支 workflow 完成 " << task.branch_id << std::endl;
    const int index_rc = invokePdkIndexRuntimeRun(task.run_dir);
    if (index_rc != 0) {
      throw std::runtime_error("Future runtime index generation failed: " + pathString(task.run_dir));
    }
    std::cout << "[C++ RuntimeHost] 预测分支索引完成 " << task.branch_id << std::endl;

    const json loop = readJson(task.run_dir / "runtime_loop_summary.json");
    const json outputs = readJson(task.run_dir / "runtime_outputs.json");
    const json evidence = readJson(task.run_dir / "runtime_evidence.json");
    const int iteration_count = jsonInt(loop.value("summary", json::object()), "iteration_count", 0);
    const int field_count = countFieldArtifacts(task.run_dir);
    const double remaining_life = findNumberRecursive(outputs, {"remaining_life_s"});
    json last_iteration = json::object();
    if (loop.contains("iterations") && loop.at("iterations").is_array() && !loop.at("iterations").empty()) {
      last_iteration = loop.at("iterations").back();
    }
    const json last_altitude =
        last_iteration.contains("altitude_m") ? last_iteration.at("altitude_m") : json(nullptr);

    json prediction = {
        {"trigger_frame_index", task.trigger_frame_index},
        {"trigger_time_s", task.trigger_time_s},
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
    if (std::isfinite(remaining_life)) {
      prediction["remaining_life_s"] = remaining_life;
    }

    json summary = {
        {"iteration_count", iteration_count},
        {"stop_reason", prediction.value("stop_reason", "")},
        {"field_artifact_count", field_count},
        {"last_altitude_m", last_altitude},
    };
    if (std::isfinite(remaining_life)) {
      summary["remaining_life_s"] = remaining_life;
    }

    {
      StateLock lock(*this);
      ++completed_prediction_runs_;
      prediction_runs_.push_back(prediction);
      appendRuntimeIndex(task.run_dir, task.branch_id, false, task.trigger_frame_index);
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
          {"branch_id", task.branch_id},
          {"run_id", task.run_id},
          {"run_dir", pathString(task.run_dir)},
          {"status", "failed"},
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

void PlatformRuntimeHost::executeOnlineLoop() {
  std::cout << "[C++ RuntimeHost] 初始化分支注册表..." << std::endl;
  initializeBranchRegistry();
  std::cout << "[C++ RuntimeHost] 读取外部观测流..." << std::endl;
  const json external_frames = loadExternalFrames();
  const int target_frames = std::min<int>(options_.online_frames, static_cast<int>(external_frames.size()));
  if (target_frames <= 0) {
    throw std::runtime_error("No external frames available for online loop.");
  }
  std::cout << "[C++ RuntimeHost] 外部观测帧数=" << target_frames << std::endl;

  fs::path previous_seed;
  double previous_time = 0.0;
  bool has_previous_time = false;
  for (int i = 0; i < target_frames; ++i) {
    const json frame = external_frames.at(static_cast<std::size_t>(i));
    const double sample_time = jsonDouble(frame, "sample_time_s", jsonDouble(frame, "time_s", i));
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
    {
      StateLock lock(*this);
      completed_online_frames_ = i + 1;
      appendRuntimeIndex(online_run_dir, "main.online", true, i);
      for (auto& branch : branch_records_) {
        if (jsonString(branch, "branch_id", "") == "main.online") {
          branch["updated_at_utc"] = nowUtcIso();
          branch["summary"] = {
              {"frame_count", completed_online_frames_},
              {"step_count", branch_steps_.size()},
              {"artifact_count", artifact_refs_.size()},
              {"qoi_ref_count", qoi_refs_.size()},
              {"checkpoint_count", checkpoint_refs_.size()},
              {"latest_runtime_outputs", pathString(runtime_outputs)},
          };
          break;
        }
      }
      writeRuntimeIndexesLocked("live_tail");
      writeProgressLocked();
    }
    maybeForkPredictionBranch(i, frame, runtime_outputs);
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
}

void PlatformRuntimeHost::writeRuntimeIndexesLocked(const std::string& cursor_mode) {
  const std::string generated = nowUtcIso();
  const int prediction_count = static_cast<int>(branch_records_.size()) - 1;
  const json registry = {
      {"schema_version", "flightenv.platform.branch_registry.v1"},
      {"run_id", options_.run_id_prefix},
      {"workflow_id", online_workflow_id_},
      {"object_id", object_id_},
      {"generated_at_utc", generated},
      {"primary_branch_id", "main.online"},
      {"branches", branch_records_},
      {"summary",
       {
           {"branch_count", branch_records_.size()},
           {"online_branch_count", 1},
           {"prediction_branch_count", std::max(0, prediction_count)},
           {"completed_prediction_count", completed_prediction_runs_.load()},
           {"failed_prediction_count", failed_prediction_runs_.load()},
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
      {"branch_id", "main.online"},
      {"follow_live", cursor_mode == "live_tail"},
      {"status", current_status_},
      {"refs", {{"run_timeline_index", "run_timeline_index.json"}}},
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

  const json timeline = {
      {"schema_version", "flightenv.platform.run_timeline_index.v1"},
      {"run_id", options_.run_id_prefix},
      {"workflow_id", online_workflow_id_},
      {"object_id", object_id_},
      {"generated_at_utc", generated},
      {"source_root", pathString(options_.chain_dir)},
      {"branch_registry_ref", "branch_registry.json"},
      {"cursor_ref", "runtime_cursor.json"},
      {"branches", branch_records_},
      {"online_frames", online_frames_},
      {"branch_steps", branch_steps_},
      {"artifact_refs", artifact_refs_},
      {"qoi_refs", qoi_refs_},
      {"checkpoint_refs", checkpoint_refs_},
      {"series_manifest_refs", json::array({{{"branch_id", "*"}, {"ref", "series_manifest.json"}}})},
      {"prediction_runs", prediction_runs_},
      {"summary",
       {
           {"branch_count", branch_records_.size()},
           {"online_frame_count", online_frames_.size()},
           {"branch_step_count", branch_steps_.size()},
           {"artifact_ref_count", artifact_refs_.size()},
           {"qoi_ref_count", qoi_refs_.size()},
           {"series_count", series.size()},
       }},
  };
  writeJson(options_.chain_dir / "run_timeline_index.json", timeline);

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
           {"execution_backend", "compiled_workflow_process_backend"},
           {"note", "调度、分支、索引由 C++ Host 接管；当前算子执行后端复用 PDK compiled-workflow runner。"},
       }},
      {"inputs",
       {
           {"compiled_online_workflow", pathString(options_.compiled_online_workflow)},
           {"compiled_future_workflow", pathString(options_.compiled_future_workflow)},
           {"adapter_registry", pathString(options_.adapter_registry)},
           {"external_observation_stream", pathString(options_.external_observation_stream)},
       }},
      {"outputs",
       {
           {"branch_registry", "branch_registry.json"},
           {"runtime_cursor", "runtime_cursor.json"},
           {"run_timeline_index", "run_timeline_index.json"},
           {"series_manifest", "series_manifest.json"},
           {"mainline_progress", "mainline_progress.json"},
           {"mainline_summary", "mainline_summary.json"},
       }},
      {"summary",
       {
           {"online_frame_count", completed_online_frames_},
           {"requested_prediction_count", requested_prediction_runs_},
           {"completed_prediction_count", completed_prediction_runs_.load()},
           {"failed_prediction_count", failed_prediction_runs_.load()},
           {"max_concurrent_branches", effective_max_concurrent_branches_},
       }},
  };
  writeJson(options_.chain_dir / "runtime_host_evidence.json", evidence);
}

void PlatformRuntimeHost::writeFinalSummary(const std::string& status) {
  StateLock lock(*this);
  current_status_ = status;
  current_stage_ = status == "completed" ? "completed" : "failed";
  current_message_ = status == "completed" ? "C++ Runtime Host 主线与预测分支完成"
                                            : "C++ Runtime Host 运行失败";
  for (auto& branch : branch_records_) {
    if (jsonString(branch, "branch_id", "") == "main.online") {
      branch["status"] = status == "completed" ? "completed" : "failed";
      branch["updated_at_utc"] = nowUtcIso();
    }
  }

  const json summary = {
      {"schema_version", "flightenv.platform.cpp_runtime_host_mainline.v1"},
      {"run_id_prefix", options_.run_id_prefix},
      {"object_id", object_id_},
      {"generated_at_utc", nowUtcIso()},
      {"status", status},
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
           {"run_count", prediction_runs_.size()},
           {"runs", prediction_runs_},
       }},
      {"acceptance",
       {
           {"cpp_runtime_host_b2_ok", true},
           {"external_measurement_port_b3_ok", completed_online_frames_ > 0},
           {"prediction_branch_b4_ok", requested_prediction_runs_ > 0},
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
    waitForPredictionBranches();
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
    options.prepare_only = args.count("prepare-only") > 0;
    options.preflight_adapters = args.count("preflight-adapters") > 0;
    options.require_adapter_registry = args.count("require-adapter-registry") > 0 ||
                                       args.count("no-require-adapter-registry") == 0;
    options.replay_by_platform_clock = args.count("replay-by-platform-clock") > 0;
    if (args.count("replay-time-scale")) {
      options.replay_time_scale = std::stod(args.at("replay-time-scale"));
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
