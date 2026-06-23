#pragma once

/**
 * @file RuntimeTimelineMaterializer.hpp
 * @brief 声明运行时间线物化工具。
 *
 * 这个组件把 runtime_loop_summary 和 data_plane_manifest 中的通用 evidence
 * 规范化为 UI、回放和审计共同消费的 timeline 条目。它被 RuntimePublicFrameBuilder
 * 和 PlatformRuntimeHost 使用，只整理 branch、step、time、port、contract 等平台字段，
 * 不决定某个对象页面应该如何渲染。
 */

#include <nlohmann/json.hpp>

#include <string>

namespace FlightEnvPlatformRuntime {

class RuntimeTimelineMaterializer {
 public:
  static nlohmann::json makeBranchStep(
      const nlohmann::json& iteration,
      const std::string& branch_id,
      int fallback_step_index);

  static nlohmann::json makeArtifactRef(
      const nlohmann::json& data_plane_entry,
      const std::string& branch_id);

  static bool isQoiRef(const nlohmann::json& timeline_ref);
};

}  // namespace FlightEnvPlatformRuntime
