#pragma once

/**
 * @file RuntimeEventLoop.hpp
 * @brief 平台 runtime 的事件循环门面。
 *
 * RuntimeEventQueue 只负责排序；RuntimeEventLoop 负责“下一件事是什么、已经派发了多少、
 * 哪类事件被处理过”这类循环层证据。它保持平台中性，不包含对象、物理或 UI 语义。
 */

#include "FlightEnvPlatformRuntime/time/RuntimeEventQueue.hpp"

#include <map>
#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 单线程事件循环门面，后续可在这里接入暂停、恢复、限速和事件审计。
 */
class RuntimeEventLoop {
 public:
  RuntimeEventQueue& queue();
  const RuntimeEventQueue& queue() const;

  bool empty() const;
  std::size_t pendingCount() const;

  void push(RuntimeEvent event);
  RuntimeEvent next();

  /**
   * @brief 将一个已派发 runtime event 规范化为 scheduler evidence 记录。
   *
   * RuntimeEventQueue 只提供通用 eventEvidence；这里补上 scheduler/event-loop
   * 层统一需要的时间戳、runtime_event_* 别名和可覆盖字段，避免宿主循环为每类事件
   * 重复拼 JSON。
   */
  static nlohmann::json schedulerEvent(
      const RuntimeEvent& event,
      const std::string& event_name = "",
      nlohmann::json extra = nlohmann::json::object());

  nlohmann::json summary() const;

 private:
  RuntimeEventQueue queue_;
  std::size_t dispatched_count_ = 0;
  std::map<std::string, std::size_t> dispatched_by_kind_;
};

}  // namespace FlightEnvPlatformRuntime
