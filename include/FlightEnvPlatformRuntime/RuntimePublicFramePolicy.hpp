#pragma once

/**
 * @file RuntimePublicFramePolicy.hpp
 * @brief 声明公开帧策略。
 *
 * 这个文件回答“public_tick 到来时，哪些内容应该进入公开 evidence，
 * held/carry-forward 应该如何表达，停止状态如何计算”。它被
 * RuntimePublicFrameBuilder 调用，使用 RuntimeEventQueue、RuntimeTimeTypes
 * 和 RuntimeNodeClockState 提供的通用运行时信息，不读取对象包语义。
 */

#include "FlightEnvPlatformRuntime/time/RuntimeEventQueue.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeNodeClockState.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace FlightEnvPlatformRuntime {

struct RuntimeHeldOutputRequest {
  const nlohmann::json* node = nullptr;            ///< 节点编译信息。
  const nlohmann::json* data_plane_info = nullptr; ///< 节点数据面端口声明。
  const nlohmann::json* port_store_refs = nullptr; ///< 端口存储中的最近 packet/ref 摘要。
  const RuntimeNodeClockState* state = nullptr;    ///< 节点最近一次执行状态；未执行时为空。
  const RuntimeEvent* event = nullptr;             ///< 触发本次公开帧的事件。
  const RuntimeLoopTick* tick = nullptr;           ///< 当前公开 tick。
  std::string timestamp_utc;                       ///< evidence 生成时间。
  std::string reason;                              ///< held 原因。
  int dispatch_tick_index = 0;                     ///< 本公开帧内的派发序号。
  bool has_output = false;                         ///< 是否存在可携带的历史输出。
};

class RuntimePublicFramePolicy {
 public:
  static nlohmann::json stopStatus(
      const nlohmann::json& outputs,
      int iteration_index,
      const nlohmann::json& loop_policy);

  static nlohmann::json makeLoopStartEvent(
      const RuntimeEvent& event,
      const RuntimeLoopTick& tick,
      const nlohmann::json& external_observation,
      const std::string& timestamp_utc);

  static nlohmann::json makeLoopFinishEvent(
      const RuntimeEvent& event,
      const RuntimeLoopTick& tick,
      const nlohmann::json& stop_status,
      int held_output_count,
      int public_tick_failed_nodes,
      double output_period_s,
      const std::string& timestamp_utc);

  static nlohmann::json makeHeldOutput(const RuntimeHeldOutputRequest& request);

  static nlohmann::json makeHeldSchedulerEvent(const RuntimeHeldOutputRequest& request);

  static nlohmann::json outputPortPolicy(
      const nlohmann::json& node,
      const nlohmann::json& data_plane_info,
      bool has_output,
      const std::string& reason,
      const nlohmann::json& last_execute_result = nlohmann::json::object(),
      const nlohmann::json& port_store_refs = nlohmann::json::object());

  static void decorateStopStatus(
      nlohmann::json& stop_status,
      const RuntimeEvent& event,
      const RuntimeLoopTick& tick,
      const nlohmann::json& effective_delta_t_s_by_node,
      const nlohmann::json& held_outputs,
      int public_tick_failed_nodes,
      double base_dt_s,
      double output_period_s,
      int node_event_count);
};

}  // namespace FlightEnvPlatformRuntime
