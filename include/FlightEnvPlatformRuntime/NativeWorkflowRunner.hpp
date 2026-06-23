#pragma once

/**
 * @file NativeWorkflowRunner.hpp
 * @brief 声明本地执行一个平台 workflow 的运行器。
 *
 * 大概：这是 platform-runtime 把对象包、workflow、profile 和 adapter 组织起来跑一次的主入口。
 * 具体：它对外提供 load/run/step/result 等接口，对内连接调度、时间、端口、evidence 和数据面。
 * 被谁使用：被 PlatformRuntimeHost、runtime CLI、测试和 launcher/SDK 间接使用。
 * 使用谁：使用 PDK WorkflowSpec、RuntimePacket、time 组件、JSON 配置和文件系统。
 * 拆分判断：职责能总结但文件偏核心；如果继续增长，应把后端执行、时间对齐、物化输出拆成独立服务。
 */


#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 原生工作流运行器的构造选项。
 *
 * 路径可以是绝对路径，也可以由调用方在构造前完成解析。运行器读取编译后的
 * 工作流元数据和适配器注册表文件，并为后续运行请求拥有适配器会话复用。
 */
struct NativeWorkflowOptions {
  std::string typed_buffer_persistence = "shadow_artifact"; ///< shadow_artifact/memory_only; controls typed-buffer replay shadow copies.
  std::filesystem::path workspace_root;      ///< 用于解析生成制品和相对引用的工作区根目录。
  std::filesystem::path pdk_root;            ///< 用于 schema、codegen 和辅助工具发现的平台 PDK 根目录。
  std::filesystem::path compiled_workflow;   ///< 包含待执行的编译后 workflow 快照的目录。
  std::filesystem::path adapter_registry;    ///< 用于将 workflow 节点绑定到后端的适配器注册表 JSON。
  std::string python = "python";             ///< 调用脚本的兼容适配器所使用的 Python 可执行程序。
  std::string runtime_zero_copy_mode = "auto"; ///< off/prefer/require/auto；控制同进程 typed buffer 快路径。
  bool require_adapter_registry = true;       ///< 为真时，缺少适配器注册表元数据会导致构造失败。
  bool allow_wildcard_adapter = false;        ///< 仅为显式允许的冒烟路径启用后备适配器绑定。
};

/**
 * @brief 一次工作流执行请求。
 *
 * 一个运行器实例可以针对同一个编译后工作流服务多次请求。每次请求的路径
 * 和 id 定义证据命名空间与输入 seed；适配器会话仍由运行器拥有。
 */
struct NativeWorkflowRequest {
  std::filesystem::path run_dir;                      ///< 写入运行时证据和制品的目录。
  std::string run_id;                                 ///< 嵌入证据记录的稳定 run 标识。
  std::string branch_id;                              ///< 可选分支标识；为空时运行器用 run_id 作为平台默认分支。
  std::string timeline_id;                            ///< 可选时间线标识；为空时使用 workflow_id 派生的默认时间线。
  std::filesystem::path seed_runtime_outputs;         ///< 可选的历史运行时输出，用作初始输入状态。
  std::filesystem::path external_observation_stream;  ///< workflow 消费的可选外部输入流。
  int max_iterations = 1;                             ///< 运行时循环迭代上限；小于一的值由实现收口。
  bool prepare_only = false;                          ///< 只初始化并校验会话，不执行派发。
};

/**
 * @brief 原生工作流运行返回的摘要。
 */
struct NativeWorkflowResult {
  int exit_code = 0;                                  ///< 进程风格退出码；零表示请求已完成。
  int iteration_count = 0;                            ///< 运行器尝试执行的循环迭代次数。
  int failed_node_count = 0;                          ///< 请求期间失败的节点派发数量。
  nlohmann::json summary = nlohmann::json::object();  ///< 包含后端和运行时引用的 JSON 证据摘要。
};

/**
 * @brief 用于 C++ 运行时宿主的原生进程内工作流执行器。
 *
 * 运行器读取编译后的工作流，调度其中的节点，拥有适配器会话生命周期，并写入
 * 运行时证据；它不会为每个帧或分支创建新的 PDK 工作流进程。进程内适配器
 * 会在每个运行器实例中加载一次，并在初始化和预热后跨 `run()` 调用复用。
 *
 * @note 边界：运行器只协调平台契约和数据面引用；算法保留在适配器会话之后。
 * @note 线程安全：运行器实例不会为并发 `run()` 调用做内部同步。请使用单一拥有
 * 线程，或在外部同步。
 */
class NativeWorkflowRunner {
 public:
  /**
   * @brief 为编译后的工作流构造运行器。
   * @param options 工作流、工作区和适配器注册表选项。
   * @throws std::runtime_error 当必需的工作流或适配器元数据无法加载时抛出。
   */
  explicit NativeWorkflowRunner(NativeWorkflowOptions options);

  /** @brief 释放拥有的适配器会话和实现状态。 */
  ~NativeWorkflowRunner();

  NativeWorkflowRunner(const NativeWorkflowRunner&) = delete;
  NativeWorkflowRunner& operator=(const NativeWorkflowRunner&) = delete;

  /**
   * @brief 针对一次请求执行或准备编译后的工作流。
   * @param request 单次 run 的证据目录、输入 seed 和迭代限制。
   * @return 完成摘要和进程风格退出码。
   */
  NativeWorkflowResult run(const NativeWorkflowRequest& request);

  /**
   * @brief 返回运行器聚合的适配器会话证据。
   * @return 描述会话协议、状态和复用情况的 JSON 对象。
   */
  nlohmann::json sessionSummary() const;

  /**
   * @brief 显式关闭所有拥有的适配器会话。
   *
   * 应在正在进行的 `run()` 调用完成后调用。析构函数也会释放拥有的实现状态。
   */
  void shutdown();

 private:
  /** @brief 拥有工作流元数据和适配器会话的不透明实现。 */
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // 命名空间 FlightEnvPlatformRuntime
