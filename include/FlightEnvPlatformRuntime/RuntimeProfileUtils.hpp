#pragma once

/**
 * @file RuntimeProfileUtils.hpp
 * @brief 读取对象 runtime profile 中通用运行策略的小工具。
 *
 * 大概：PlatformRuntimeHost 需要使用对象包声明的停止原因、摘要指标和健康指标。
 * 具体：这里只解释 profile 的通用字段形状，不写入任何具体对象语义；对象含义留在对象包 JSON 中。
 * 被谁使用：被 PlatformRuntimeHost 等 runtime 聚合层使用。
 * 使用谁：使用 nlohmann::json 和标准库容器，不依赖具体算子或对象模型。
 */

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 从对象 profile 中收集会让分支自然结束的 stop_reason。
 * @param profile 对象包提供的 runtime profile。
 * @return 停止原因字符串列表；为空表示对象包未声明终止原因。
 */
std::vector<std::string> terminalStopReasonsFromProfile(const nlohmann::json& profile);

/**
 * @brief 判断 stop_reason 是否匹配对象 profile 声明的终止策略。
 * @param stop_reason runtime loop 输出的停止原因。
 * @param profile 对象包提供的 runtime profile。
 * @return 匹配时返回 true；未声明或不匹配时返回 false。
 */
bool stopReasonMatchesTerminalPolicy(const std::string& stop_reason, const nlohmann::json& profile);

/**
 * @brief 按对象 profile 声明抽取指标并生成一组输出字段。
 *
 * profile 路径为 `health_ledger.<section_key>`，每个条目可声明：
 * `metric_id` 或 `metric_key_group`、`output_key`、可选 `keys` 和 `emit_null`。
 *
 * @param primary 优先查找的扁平 JSON 对象。
 * @param secondary 递归查找的 JSON 对象或数组。
 * @param profile 对象包提供的 runtime profile。
 * @param section_key profile.health_ledger 下的指标声明数组名。
 * @return 可直接 merge 到 evidence/summary 的 JSON 对象。
 */
nlohmann::json collectConfiguredMetrics(const nlohmann::json& primary,
                                        const nlohmann::json& secondary,
                                        const nlohmann::json& profile,
                                        const std::string& section_key);

}  // namespace FlightEnvPlatformRuntime
