#pragma once

/**
 * @file RuntimeNodeClockState.hpp
 * @brief 声明每个节点自己的时钟状态。
 *
 * 大概：这是平台记住某个节点上次何时运行、下次何时到期、输出是否过期的小账本。
 * 具体：它服务于非整数频率、多速率调度和事件队列生成。
 * 被谁使用：被 RuntimeTimeScheduler、RuntimeEventQueue 和时间调度测试使用。
 * 使用谁：使用 RuntimeTimeTypes 和节点 id 等通用字段。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */


#include "FlightEnvPlatformRuntime/time/RuntimeTimeTypes.hpp"

#include <limits>
#include <nlohmann/json.hpp>

namespace FlightEnvPlatformRuntime {

/**
 * @brief `RuntimeTimeScheduler` 保留的逐节点时钟状态。
 *
 * 该状态只记录平台派发时间和保持输出上下文。它不是适配器状态快照，
 * 也不应承载对象特有语义。
 */
struct RuntimeNodeClockState {
  bool has_executed = false; ///< 调度器记录该节点首次完成派发后为真。
  int last_execution_iteration = -1; ///< 上一次完成派发的公开迭代索引。
  RuntimeTimePoint last_execution_public_time; ///< 上一次派发的内部公开时间点。
  RuntimeTimePoint next_due_public_time; ///< 已知时的下一次内部到期时间点。
  double last_execution_public_time_s = std::numeric_limits<double>::quiet_NaN(); ///< 上一次派发的公开时间。
  double next_due_public_time_s = std::numeric_limits<double>::quiet_NaN(); ///< 已知时的下一次公开到期时间。
  nlohmann::json last_execute_result = nlohmann::json::object(); ///< 为保持输出复用保留的上一次输出包。
  nlohmann::json last_time_info = nlohmann::json::object(); ///< 为证据保留的上一次派发 time_info。
};

/**
 * @brief 节拍状态命名的向后兼容别名。
 */
using RuntimeNodeCadenceState = RuntimeNodeClockState;

}  // 命名空间 FlightEnvPlatformRuntime
