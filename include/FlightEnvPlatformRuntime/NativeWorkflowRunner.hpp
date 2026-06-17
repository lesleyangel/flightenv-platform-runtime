#pragma once

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

struct NativeWorkflowOptions {
  std::filesystem::path workspace_root;
  std::filesystem::path pdk_root;
  std::filesystem::path compiled_workflow;
  std::filesystem::path adapter_registry;
  std::string python = "python";
  bool require_adapter_registry = true;
  bool allow_wildcard_adapter = false;
};

struct NativeWorkflowRequest {
  std::filesystem::path run_dir;
  std::string run_id;
  std::filesystem::path seed_runtime_outputs;
  std::filesystem::path external_observation_stream;
  int max_iterations = 1;
  bool prepare_only = false;
};

struct NativeWorkflowResult {
  int exit_code = 0;
  int iteration_count = 0;
  int failed_node_count = 0;
  nlohmann::json summary = nlohmann::json::object();
};

// NativeWorkflowRunner 是 C++ Runtime Host 的原生 workflow 执行器。
//
// 这里的“原生”含义是：Host 自己读取 compiled workflow、调度节点、管理
// adapter 生命周期，不再为每一帧/每个预测分支启动一次 PDK workflow 进程。
// DLL adapter 会在本进程中 LoadLibrary + create 一次，并在后续 run() 调用中复用
// initialize/warmup 后的句柄。CLI/ROS2 adapter 的生命周期也由 Host 识别和记录；
// 当前真实 reentry 对象包走 plugin_dll/dll_abi.v1 路径。
class NativeWorkflowRunner {
 public:
  explicit NativeWorkflowRunner(NativeWorkflowOptions options);
  ~NativeWorkflowRunner();

  NativeWorkflowRunner(const NativeWorkflowRunner&) = delete;
  NativeWorkflowRunner& operator=(const NativeWorkflowRunner&) = delete;

  NativeWorkflowResult run(const NativeWorkflowRequest& request);
  nlohmann::json sessionSummary() const;
  void shutdown();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace FlightEnvPlatformRuntime
