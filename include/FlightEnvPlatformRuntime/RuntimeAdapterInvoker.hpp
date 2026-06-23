#pragma once

/**
 * @file RuntimeAdapterInvoker.hpp
 * @brief 平台 runtime 的 adapter 执行入口。
 *
 * NativeWorkflowRunner 决定节点何时执行，本组件负责“如何安全调用 adapter”：
 * 组装 execute 调用、物化 typed 输出、执行零拷贝策略检查、校验输出端口契约。
 * 这样 typed ABI/typed buffer 的演进不会继续分散在 runner 热路径里。
 */

#include "FlightEnvPlatformRuntime/AdapterSession.hpp"

#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 一次 adapter execute 调用所需的运行时上下文。
 */
struct RuntimeAdapterInvokeRequest {
  std::filesystem::path run_dir;               ///< 当前 run package 目录。
  nlohmann::json context = nlohmann::json::object();
  nlohmann::json upstream = nlohmann::json::object();
  nlohmann::json data_plane_info = nlohmann::json::object();
  nlohmann::json time_info = nlohmann::json::object();
  int iteration_index = 0;
  std::string node_id;
  std::string runtime_zero_copy_mode = "auto";
  std::string typed_buffer_persistence = "shadow_artifact";
  std::function<void(const std::string&)> trace;
};

/**
 * @brief 执行 adapter，并统一处理 typed output / zero-copy / contract validation。
 */
class RuntimeAdapterInvoker {
 public:
  static nlohmann::json execute(IAdapterSession& session, const RuntimeAdapterInvokeRequest& request);

  static void validateOutputContracts(
      const nlohmann::json& data_plane_info,
      const nlohmann::json& execute_result,
      const std::string& node_id,
      bool allow_inline_typed_outputs);
};

}  // namespace FlightEnvPlatformRuntime
