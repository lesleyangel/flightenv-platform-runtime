#pragma once

/**
 * @file PlatformRuntimeHost.hpp
 * @brief 声明平台运行宿主的 runtime 实现门面。
 *
 * 大概：这是上层程序不用关心内部 runner 细节时调用的平台入口。
 * 具体：它负责加载对象包、启动/停止运行、推进主线、查询分支和收集 evidence。
 * 被谁使用：被 runtime exe、launcher、SDK、UI 集成层和端到端测试使用。
 * 使用谁：使用 NativeWorkflowRunner、RuntimeTimeScheduler、PDK runtime/evidence 契约。
 * 拆分判断：职责能总结但容易变大；新增分支、证据或 UI 兼容逻辑时应拆到子服务。
 */


#include <atomic>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "FlightEnvPlatformRuntime/RuntimeEventLoop.hpp"

namespace FlightEnvPlatformRuntime {

class NativeWorkflowRunner;

/**
 * @brief 运行时宿主的命令与执行选项。
 *
 * 这些选项描述平台根目录、编译后工作流输入、适配器绑定元数据、执行限制、
 * 分支工作进程状态和输出位置。它们是普通值，便于在构造
 * `PlatformRuntimeHost` 前由 CLI 解析或测试填充。
 */
struct HostOptions {
  std::string typed_buffer_persistence = "shadow_artifact"; ///< shadow_artifact/memory_only; controls typed-buffer replay shadow copies.
  std::filesystem::path workspace_root;              ///< 共享制品和默认路径解析使用的工作区根目录。
  std::filesystem::path pdk_root;                    ///< 包含 validator、schema 和辅助工具的平台 PDK 根目录。
  std::filesystem::path object_package_root;         ///< 用于发现运行时 profile 的对象包根目录。
  std::filesystem::path compiled_online_workflow;    ///< 主在线分支使用的编译后 workflow 目录。
  std::filesystem::path compiled_future_workflow;    ///< 预测分支执行使用的编译后 workflow 目录。
  std::filesystem::path adapter_registry;            ///< 将编译后节点绑定到后端的适配器注册表 JSON。
  std::filesystem::path external_observation_stream; ///< 在线主分支消费的外部输入流。
  std::filesystem::path pdk_run_script;              ///< 旧进程后端使用的兼容运行脚本。
  std::filesystem::path pdk_cli;                     ///< 兼容路径使用的平台 CLI 可执行程序或脚本。
  std::filesystem::path run_root;                    ///< 派生 chain_dir 时用于单次 run 证据的根目录。
  std::filesystem::path chain_dir;                   ///< 保存宿主级索引、分支注册表和摘要的目录。
  std::string python = "python";                     ///< 兼容脚本使用的 Python 可执行程序。
  std::string execution_backend = "native_adapter_sessions"; ///< CLI 或测试请求的后端模式。
  std::string runtime_zero_copy_mode = "auto";       ///< off/prefer/require/auto；同进程 adapter typed buffer 快路径策略。
  std::string run_id_prefix = "cpp_runtime_host_online_to_rul"; ///< 用于派生稳定运行 id 的前缀。
  int online_frames = 0;                             ///< 请求的在线输入帧数；默认值稍后解析。
  int prediction_every_frames = 0;                   ///< 自动预测分支的帧间隔；零表示使用 profile 默认值。
  int future_max_iterations = 0;                     ///< 每个预测分支的迭代限制。
  int max_concurrent_branches = 0;                   ///< 分支并发限制；零表示使用 profile 默认值。
  int branch_chunk_iterations = 10;                  ///< 分支工作进程恢复时使用的迭代块大小。
  bool prepare_only = false;                         ///< 准备输入和会话、写入证据，并跳过派发。
  bool preflight_adapters = false;                   ///< 在正常执行前启用适配器校验。
  bool require_adapter_registry = true;              ///< 必需的适配器注册表元数据缺失时快速失败。
  bool allow_legacy_process_backend = false;         ///< 允许使用兼容进程执行而不是原生会话。
  bool replay_by_platform_clock = false;             ///< 按平台墙钟时间回放外部输入。
  bool branch_manager_enabled = true;                ///< 使用分支工作进程和持久化分支管理状态。
  bool branch_worker = false;                        ///< 将当前进程作为预测分支工作进程运行。
  bool resume_existing_branch = false;               ///< 从已有分支管理记录恢复分支状态。
  bool wait_for_prediction_branches = false;         ///< 使主进程保持运行，直到已启动分支完成。
  double replay_time_scale = 0.0;                    ///< 墙钟回放倍率；非正值选择快速/事件驱动模式。
  std::string branch_control_action;                 ///< 可选的分支管理命令，用于替代常规执行。
  std::string branch_id;                             ///< 工作进程或控制操作使用的分支标识。
  std::string branch_run_id;                         ///< 分配给分支工作进程请求的运行 id。
  std::string trigger_event_id;                      ///< 触发预测分支的运行时事件 id。
  std::filesystem::path branch_run_dir;              ///< 分支工作进程请求的证据目录。
  std::filesystem::path seed_runtime_outputs;        ///< 用于 seed 分支或恢复运行的运行时输出。
  std::filesystem::path runtime_host_exe;            ///< 用于启动分支工作进程的可执行程序路径。
  int trigger_frame_index = -1;                      ///< 触发分支的主线帧索引；未设置时为 -1。
  double trigger_time_s = 0.0;                       ///< 与分支触发关联的主线时间。
};

/**
 * @brief 用于平台在线执行和预测分支管理的 C++ 宿主。
 *
 * `PlatformRuntimeHost` 接收编译后的工作流和适配器注册表，将外部输入帧
 * 注入主时间线，调度后台预测分支，并聚合证据。它不实现对象算法；算子行为
 * 保留在适配器会话和编译后工作流契约之后。
 *
 * @note 生命周期：使用完整填充的 `HostOptions` 构造，调用一次 `run()`，然后销毁宿主。
 * 分支工作进程由使用工作进程选项的独立宿主调用表示。
 * @note 线程安全：分支线程运行期间，宿主会保护部分共享证据状态；但公开宿主对象
 * 不设计为支持并发调用 `run()`。
 */
class PlatformRuntimeHost {
 public:
  /**
   * @brief 根据命令或测试选项创建宿主。
   * @param options 运行时输入、后端选择、分支设置和输出路径。
   */
  explicit PlatformRuntimeHost(HostOptions options);

  /**
   * @brief 运行请求的宿主模式。
   * @return 进程风格退出码；零表示请求的宿主模式已完成，非零表示设置或主线执行失败。
   */
  int run();

 private:
  /**
   * @brief 单个预测分支请求的内部描述。
   */
  struct PredictionTask {
    std::string branch_id;                      ///< 宿主索引和分支管理状态使用的分支 id。
    std::string run_id;                         ///< 分配给分支执行的运行 id。
    std::filesystem::path run_dir;              ///< 当前分支的证据目录。
    std::filesystem::path seed_runtime_outputs; ///< 用作分支 seed 的运行时输出。
    int trigger_frame_index = 0;                ///< 触发分支的主线帧索引。
    double trigger_time_s = 0.0;                ///< 主线触发时间，单位为秒。
    std::string trigger_event_id;               ///< 导致分支创建的运行时事件 id。
  };

  /**
   * @brief 宿主原子自旋锁的小型 RAII guard。
   */
  class StateLock {
   public:
    /**
     * @brief 获取宿主状态锁。
     * @param host 共享证据状态将被保护的宿主。
     */
    explicit StateLock(PlatformRuntimeHost& host);

    /** @brief 释放宿主状态锁。 */
    ~StateLock();
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;

   private:
    PlatformRuntimeHost& host_;
  };

  HostOptions options_;
  nlohmann::json online_workflow_snapshot_;
  nlohmann::json future_workflow_snapshot_;
  nlohmann::json object_runtime_profile_;
  std::string online_workflow_id_;
  std::string future_workflow_id_;
  std::string object_id_;
  bool automatic_branching_enabled_ = false;
  std::string branch_trigger_kind_ = "disabled";
  int effective_prediction_every_frames_ = 0;
  double effective_prediction_time_interval_s_ = 0.0;
  double last_prediction_trigger_time_s_ = 0.0;
  bool has_prediction_trigger_time_ = false;
  int effective_max_concurrent_branches_ = 1;

  std::atomic_flag state_lock_ = ATOMIC_FLAG_INIT;
  nlohmann::json branch_records_ = nlohmann::json::array();
  nlohmann::json online_frames_ = nlohmann::json::array();
  nlohmann::json branch_steps_ = nlohmann::json::array();
  nlohmann::json artifact_refs_ = nlohmann::json::array();
  nlohmann::json qoi_refs_ = nlohmann::json::array();
  nlohmann::json checkpoint_refs_ = nlohmann::json::array();
  nlohmann::json prediction_runs_ = nlohmann::json::array();
  nlohmann::json runtime_events_ = nlohmann::json::array();
  RuntimeEventLoop runtime_event_loop_;
  nlohmann::json series_by_id_ = nlohmann::json::object();
  std::vector<std::thread> branch_threads_;
  std::atomic<int> completed_prediction_runs_{0};
  std::atomic<int> failed_prediction_runs_{0};
  int requested_prediction_runs_ = 0;
  int completed_online_frames_ = 0;
  std::string latest_prediction_branch_id_;
  double current_run_time_s_ = 0.0;
  double current_source_time_s_ = 0.0;
  double current_delta_t_s_ = 0.0;
  int current_tick_index_ = 0;
  std::string current_stage_ = "initializing";
  std::string current_status_ = "running";
  std::string current_message_ = "";
  std::unique_ptr<NativeWorkflowRunner> online_native_runner_;

  void resolveDefaults();
  void loadCompiledWorkflowMetadata();
  void ensureInputs() const;
  void prepareRuntime();
  void executeOnlineLoop();
  void waitForPredictionBranches();
  int runBranchWorker();
  int applyBranchControl();
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
      const std::filesystem::path& online_runtime_outputs,
      const std::string& posterior_event_id);
  void runPredictionBranch(PredictionTask task);
  void launchPredictionBranchWorker(const PredictionTask& task);

  void initializeBranchRegistry();
  void upsertBranchRecord(const nlohmann::json& record);
  void updateBranchStatus(
      const std::string& branch_id,
      const std::string& status,
      const nlohmann::json& summary);
  std::string appendRuntimeEventLocked(
      const std::string& event_kind,
      const std::string& branch_id,
      int frame_index,
      double time_s,
      const nlohmann::json& payload);
  RuntimeEvent dispatchRuntimeEventLocked(RuntimeEvent event);
  void appendRuntimeIndex(
      const std::filesystem::path& run_dir,
      const std::string& branch_id,
      bool online_branch,
      int mainline_frame_index,
      double mainline_time_origin_s);
  void mergeSeriesManifest(const std::filesystem::path& manifest_path, const std::string& branch_id);
  void writeRuntimeBranchIndex(const std::filesystem::path& run_dir) const;
  void writeRuntimeIndexesLocked(const std::string& cursor_mode);
  void writeProgressLocked();
  void writeRuntimeHostEvidenceLocked(const std::string& status);
  nlohmann::json buildHealthLedgerLocked() const;
  void writeHealthLedgerLocked() const;
  bool useNativeBackend() const;
  NativeWorkflowRunner& onlineNativeRunner();
  void runCompiledWorkflow(
      const std::filesystem::path& compiled_workflow,
      NativeWorkflowRunner* runner,
      const std::filesystem::path& run_dir,
      const std::string& run_id,
      const std::filesystem::path& seed_runtime_outputs,
      const std::filesystem::path& external_observation_stream,
      int max_iterations,
      bool prepare_only,
      const std::string& branch_id = "",
      const std::string& timeline_id = "");

  int invokePdkRun(
      const std::filesystem::path& compiled_workflow,
      const std::filesystem::path& run_dir,
      const std::string& run_id,
      const std::filesystem::path& seed_runtime_outputs,
      const std::filesystem::path& external_observation_stream,
      int max_iterations,
      bool prepare_only) const;
};

/**
 * @brief 解析 CLI 参数并运行 `PlatformRuntimeHost`。
 * @param argc 来自 `main` 的参数数量。
 * @param argv 来自 `main` 的参数向量；其中的值会解析为宿主选项。
 * @return 由 CLI 解析或宿主执行产生的进程风格退出码。
 */
int RunPlatformRuntimeHostCli(int argc, char** argv);

}  // 命名空间 FlightEnvPlatformRuntime
