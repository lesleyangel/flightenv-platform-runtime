#pragma once

/**
 * @file RuntimeBranchService.hpp
 * @brief 平台 runtime 的分支记录服务。
 *
 * 这个服务只维护分支记录、分支事件和通用状态字段，不理解对象里的预测、失效或健康含义。
 * PlatformRuntimeHost 仍决定何时触发分支；事件和记录的稳定格式放在这里统一。
 */

#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 分支注册表和事件账本的通用操作。
 */
class RuntimeBranchService {
 public:
  static void upsertRecord(nlohmann::json& records, const nlohmann::json& record);

  static bool updateRecordStatus(
      nlohmann::json& records,
      const std::string& branch_id,
      const std::string& status,
      const nlohmann::json& summary);

  static std::string appendEvent(
      nlohmann::json& events,
      const std::string& run_id_prefix,
      const std::string& event_kind,
      const std::string& branch_id,
      int frame_index,
      double time_s,
      const nlohmann::json& payload);
};

}  // namespace FlightEnvPlatformRuntime
