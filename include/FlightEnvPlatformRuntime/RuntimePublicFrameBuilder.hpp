#pragma once

/**
 * @file RuntimePublicFrameBuilder.hpp
 * @brief 根据事件驱动的节点输出生成公开帧。
 *
 * 这个组件被 NativeWorkflowRunner 使用，负责把一次 public_tick 中新产生的
 * 节点输出、held/carry-forward 输出、停止状态快照和调度证据组织成平台公开帧。
 * 它只使用 RuntimeTimeScheduler 和 RuntimeEventQueue 提供的通用时间/事件信息，
 * 不理解具体对象物理语义，也不直接服务某个 UI 页面。
 */

#include "FlightEnvPlatformRuntime/RuntimeTimeScheduler.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeEventQueue.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace flightenv::platform {
class ThreadSafePortStore;
}

namespace FlightEnvPlatformRuntime {

struct RuntimePublicFrameRequest {
  const RuntimeEvent* event = nullptr;
  const RuntimeLoopTick* tick = nullptr;
  const std::vector<nlohmann::json>* nodes = nullptr;
  const nlohmann::json* data_plane_plan = nullptr;
  const flightenv::platform::ThreadSafePortStore* port_store = nullptr;
  const RuntimeTimeScheduler* time_scheduler = nullptr;
  nlohmann::json* outputs = nullptr;
  nlohmann::json* public_tick_outputs = nullptr;
  nlohmann::json effective_delta_t_s_by_node = nlohmann::json::object();
  nlohmann::json loop_policy = nlohmann::json::object();
  nlohmann::json external_observation = nlohmann::json::object();
  std::string timestamp_utc;
  double base_dt_s = 0.0;
  double output_period_s = 0.0;
  int public_tick_failed_nodes = 0;
  int dispatch_tick_index = 0;
};

struct RuntimePublicFrameResult {
  int loop_iteration_index = 0;
  int next_dispatch_tick_index = 0;
  bool failed = false;
  nlohmann::json scheduler_events = nlohmann::json::array();
  nlohmann::json held_outputs = nlohmann::json::array();
  nlohmann::json timeline_entries = nlohmann::json::array();
  nlohmann::json stop_status = nlohmann::json::object();
};

class RuntimePublicFrameBuilder {
 public:
  static RuntimePublicFrameResult build(const RuntimePublicFrameRequest& request);
};

}  // namespace FlightEnvPlatformRuntime
