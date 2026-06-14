#pragma once

#include <atomic>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace FlightEnvPlatformRuntime {

// PlatformRuntimeHost 是生产 C++ Runtime Host 的第一版骨架。
// 它不实现具体物理算法，而是接收“已编译 workflow + adapter registry”，
// 负责在线主时间轴、外部观测帧注入、预测分支后台执行和 evidence 聚合。
struct HostOptions {
  std::filesystem::path workspace_root;
  std::filesystem::path pdk_root;
  std::filesystem::path object_package_root;
  std::filesystem::path compiled_online_workflow;
  std::filesystem::path compiled_future_workflow;
  std::filesystem::path adapter_registry;
  std::filesystem::path external_observation_stream;
  std::filesystem::path pdk_run_script;
  std::filesystem::path pdk_cli;
  std::filesystem::path run_root;
  std::filesystem::path chain_dir;
  std::string python = "python";
  std::string run_id_prefix = "cpp_runtime_host_online_to_rul";
  int online_frames = 50;
  int prediction_every_frames = 0;
  int future_max_iterations = 6;
  int max_concurrent_branches = 0;
  bool prepare_only = false;
  bool preflight_adapters = false;
  bool require_adapter_registry = true;
  bool replay_by_platform_clock = false;
  double replay_time_scale = 0.0;
};

class PlatformRuntimeHost {
 public:
  explicit PlatformRuntimeHost(HostOptions options);

  // 返回进程退出码：0 表示验收 smoke 通过，非 0 表示主线失败。
  int run();

 private:
  struct PredictionTask {
    std::string branch_id;
    std::string run_id;
    std::filesystem::path run_dir;
    std::filesystem::path seed_runtime_outputs;
    int trigger_frame_index = 0;
    double trigger_time_s = 0.0;
  };

  class StateLock {
   public:
    explicit StateLock(PlatformRuntimeHost& host);
    ~StateLock();
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;

   private:
    PlatformRuntimeHost& host_;
  };

  HostOptions options_;
  nlohmann::json online_workflow_snapshot_;
  nlohmann::json future_workflow_snapshot_;
  std::string online_workflow_id_;
  std::string future_workflow_id_;
  std::string object_id_;
  int effective_prediction_every_frames_ = 0;
  int effective_max_concurrent_branches_ = 1;

  std::atomic_flag state_lock_ = ATOMIC_FLAG_INIT;
  nlohmann::json branch_records_ = nlohmann::json::array();
  nlohmann::json online_frames_ = nlohmann::json::array();
  nlohmann::json branch_steps_ = nlohmann::json::array();
  nlohmann::json artifact_refs_ = nlohmann::json::array();
  nlohmann::json qoi_refs_ = nlohmann::json::array();
  nlohmann::json checkpoint_refs_ = nlohmann::json::array();
  nlohmann::json prediction_runs_ = nlohmann::json::array();
  nlohmann::json series_by_id_ = nlohmann::json::object();
  std::vector<std::thread> branch_threads_;
  std::atomic<int> completed_prediction_runs_{0};
  std::atomic<int> failed_prediction_runs_{0};
  int requested_prediction_runs_ = 0;
  int completed_online_frames_ = 0;
  double current_run_time_s_ = 0.0;
  double current_source_time_s_ = 0.0;
  double current_delta_t_s_ = 0.0;
  int current_tick_index_ = 0;
  std::string current_stage_ = "initializing";
  std::string current_status_ = "running";
  std::string current_message_ = "";

  void resolveDefaults();
  void loadCompiledWorkflowMetadata();
  void ensureInputs() const;
  void prepareRuntime();
  void executeOnlineLoop();
  void waitForPredictionBranches();
  void writeFinalSummary(const std::string& status);

  nlohmann::json loadExternalFrames() const;
  std::filesystem::path writeOneFrameStream(const nlohmann::json& frame, int local_index) const;
  std::filesystem::path runOneOnlineFrame(
      const nlohmann::json& frame,
      int local_index,
      const std::filesystem::path& previous_seed);
  void maybeForkPredictionBranch(
      int local_index,
      const nlohmann::json& frame,
      const std::filesystem::path& online_runtime_outputs);
  void runPredictionBranch(PredictionTask task);

  void initializeBranchRegistry();
  void upsertBranchRecord(const nlohmann::json& record);
  void updateBranchStatus(
      const std::string& branch_id,
      const std::string& status,
      const nlohmann::json& summary);
  void appendRuntimeIndex(
      const std::filesystem::path& run_dir,
      const std::string& branch_id,
      bool online_branch,
      int mainline_frame_index);
  void mergeSeriesManifest(const std::filesystem::path& manifest_path, const std::string& branch_id);
  void writeRuntimeIndexesLocked(const std::string& cursor_mode);
  void writeProgressLocked();
  void writeRuntimeHostEvidenceLocked(const std::string& status);

  int invokePdkRun(
      const std::filesystem::path& compiled_workflow,
      const std::filesystem::path& run_dir,
      const std::string& run_id,
      const std::filesystem::path& seed_runtime_outputs,
      const std::filesystem::path& external_observation_stream,
      int max_iterations,
      bool prepare_only) const;
  int invokePdkIndexRuntimeRun(const std::filesystem::path& run_dir) const;
};

int RunPlatformRuntimeHostCli(int argc, char** argv);

}  // namespace FlightEnvPlatformRuntime
