#pragma once

/**
 * @file RuntimeTypedPayloadBridge.hpp
 * @brief 将声明为 typed IO 的端口输出物化为 typed payload 引用。
 *
 * 大概：平台仍允许 adapter 初始化和 evidence 使用 JSON，但算子高频输入输出不能只有裸 JSON。
 * 具体：当端口声明 `json_operator_io_forbidden` 且显式开启迁移兼容开关时，本模块才会
 * 为 inline 输出补 typed buffer ref。生产合规判断由 RuntimeZeroCopyPolicy 负责。
 * 被谁使用：被 NativeWorkflowRunner 的执行热路径和输出契约校验使用。
 * 使用谁：使用 nlohmann::json、文件系统和平台编译后的 data-plane 端口声明。
 */

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 判断端口声明是否禁止算子 IO 只走 JSON。
 *
 * @note 保留这个函数是为了兼容已有调用点；实现委托给 RuntimeZeroCopyPolicy。
 */
bool typedIoJsonForbidden(const nlohmann::json& port_spec);

/**
 * @brief 为 typed 输出端口补充 typed payload ref。
 * @param run_dir 当前 run 的证据目录。
 * @param data_plane_info 当前节点的 data-plane 声明。
 * @param execute_result adapter 执行结果；函数会返回补充后的副本。
 * @param time_info 当前运行时钟与事件信息。
 * @param iteration_index 当前循环迭代序号。
 * @return 已为 typed 输出补 ref 的执行结果。
 */
nlohmann::json materializeTypedOutputPayloads(const std::filesystem::path& run_dir,
                                             const nlohmann::json& data_plane_info,
                                             nlohmann::json execute_result,
                                             const nlohmann::json& time_info,
                                             int iteration_index,
                                             const std::string& typed_buffer_persistence = "shadow_artifact");

}  // namespace FlightEnvPlatformRuntime
