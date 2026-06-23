#pragma once

/**
 * @file RuntimeEventQueue.hpp
 * @brief 声明按时间和优先级排序的 runtime 事件队列。
 *
 * 大概：这是统一时钟的基础组件，负责让 node_due、public_tick、input_arrived 等事件按顺序出队。
 * 具体：它只处理事件排序和弹出，不理解事件背后的对象含义。
 * 被谁使用：被 RuntimeTimeScheduler、NativeWorkflowRunner 和时间调度测试使用。
 * 使用谁：使用 RuntimeTimeTypes 和标准优先队列。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */


#include "FlightEnvPlatformRuntime/time/RuntimeTimeTypes.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <queue>
#include <string>
#include <vector>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 调度器和宿主事件循环使用的运行时事件。
 *
 * 事件按事件时间、优先级和插入顺序排序。载荷有意保持为 JSON，使平台事件
 * 元数据可以演进，而不会把此头文件耦合到对象特有数据。
 */
struct RuntimeEvent {
  std::string event_id;                         ///< 稳定事件 id；为空时在入队时生成。
  std::string event_kind;                       ///< 机器可读的事件类型。
  RuntimeTimePoint event_time;                  ///< 内部排序使用的稳定事件时间点。
  double event_time_s = 0.0;                    ///< 公开运行时时钟上的事件时间，单位为秒。
  int priority = 0;                             ///< 同一时间上数值越小越先派发。
  int iteration_index = -1;                     ///< 与此事件关联的公开迭代；不适用时为 -1。
  std::string target_id;                        ///< workflow、节点、输入或 checkpoint 的目标标识。
  std::string cause_event_id;                   ///< 用于因果证据的可选父事件 id。
  RuntimeLoopTick loop_tick;                    ///< 为公开 tick 事件填充的循环 tick。
  nlohmann::json payload = nlohmann::json::object(); ///< 用于证据和派发的可扩展事件载荷。
};

/**
 * @brief 运行时事件的稳定优先队列。
 *
 * 队列是单一拥有者的内存结构。它不提供访问同步；宿主应在一个调度线程中使用它，
 * 或在外部提供保护。
 */
class RuntimeEventQueue {
 public:
  /**
   * @brief 向队列加入事件。
   * @param event 要入队的事件。空 id 会填充为生成的 id。
   */
  void push(RuntimeEvent event);

  /**
   * @brief 为有界循环范围加入固定公开 tick 事件。
   * @param max_iterations 要预置的公开 tick 数量。
   * @param base_dt_s 基础步长，单位为秒。
   * @param output_period_s 首选公开输出周期，单位为秒。
   */
  void pushFixedPublicTicks(int max_iterations, double base_dt_s, double output_period_s);

  /**
   * @brief 检查队列是否没有待处理事件。
   * @return 队列中没有事件时为真；至少有一个待处理事件时为假。
   */
  bool empty() const;

  /**
   * @brief 统计待处理事件数量。
   * @return 当前队列中的事件数量。
   */
  std::size_t size() const;

  /**
   * @brief 移除并返回下一个事件。
   * @return 时间最早且优先级最高的队列事件。
   * @throws std::runtime_error 当在空队列上调用时抛出。
   */
  RuntimeEvent pop();

  /**
   * @brief 根据循环 tick 创建公开 tick 事件。
   * @param tick 要包装的公开循环 tick。
   * @return 以工作流为目标的事件。
   */
  static RuntimeEvent publicTickEvent(const RuntimeLoopTick& tick);

  /**
   * @brief 创建节点到期事件。
   * @param node_id 目标节点标识。
   * @param event_time_s 节点到期的公开时间。
   * @param public_iteration_index 与事件关联的公开迭代。
   * @param priority 同一事件时间内的排序优先级。
   * @param payload 证据和派发使用的可选元数据。
   * @return 以节点为目标的事件。
   */
  static RuntimeEvent nodeDueEvent(
      const std::string& node_id,
      double event_time_s,
      int public_iteration_index,
      int priority,
      nlohmann::json payload = nlohmann::json::object());

  /**
   * @brief 创建输入到达事件。
   * @param input_id 目标输入标识。
   * @param event_time_s 输入变为可用的公开时间。
   * @param public_iteration_index 与事件关联的公开迭代。
   * @param payload 可选输入元数据。
   * @return 以输入为目标的事件。
   */
  static RuntimeEvent inputArrivedEvent(
      const std::string& input_id,
      double event_time_s,
      int public_iteration_index,
      nlohmann::json payload = nlohmann::json::object());

  /**
   * @brief 创建检查点到期事件。
   * @param checkpoint_id 目标检查点标识。
   * @param event_time_s 检查点物化到期的公开时间。
   * @param public_iteration_index 与事件关联的公开迭代。
   * @param payload 可选检查点元数据。
   * @return 以检查点为目标的事件。
   */
  static RuntimeEvent checkpointDueEvent(
      const std::string& checkpoint_id,
      double event_time_s,
      int public_iteration_index,
      nlohmann::json payload = nlohmann::json::object());

  /**
   * @brief 创建停止条件检查事件。
   * @param event_time_s 停止条件应检查的公开运行时刻，单位为秒。
   * @param public_iteration_index 与检查关联的公开迭代。
   * @param payload 可选停止策略元数据。
   * @return 以 workflow 为目标的停止检查事件。
   */
  static RuntimeEvent stopCheckDueEvent(
      double event_time_s,
      int public_iteration_index,
      nlohmann::json payload = nlohmann::json::object());

  /**
   * @brief 创建分支触发事件。
   * @param branch_id 目标分支标识。
   * @param event_time_s 分支被触发的公开运行时刻，单位为秒。
   * @param public_iteration_index 与事件关联的公开迭代。
   * @param cause_event_id 触发该分支事件的上游事件 id。
   * @param payload 可选分支触发元数据。
   * @return 以分支为目标的事件。
   */
  static RuntimeEvent branchTriggeredEvent(
      const std::string& branch_id,
      double event_time_s,
      int public_iteration_index,
      const std::string& cause_event_id = "",
      nlohmann::json payload = nlohmann::json::object());

  /**
   * @brief 将 runtime 事件转换为通用 evidence JSON。
   * @param event 已归一化的 runtime 事件。
   * @return 包含秒制和纳秒制时间字段的事件证据。
   */
  static nlohmann::json eventEvidence(const RuntimeEvent& event);

 private:
  /**
   * @brief 在排序键相同的情况下保留 FIFO 顺序的队列存储项。
   */
  struct QueuedEvent {
    RuntimeEvent event;          ///< 事件载荷。
    std::uint64_t sequence = 0;  ///< 单调递增的插入序号。
  };

  /**
   * @brief 优先队列比较器，按最早时间、最低优先级值、FIFO 顺序排序。
   */
  struct EventOrder {
    bool operator()(const QueuedEvent& lhs, const QueuedEvent& rhs) const;
  };

  std::priority_queue<QueuedEvent, std::vector<QueuedEvent>, EventOrder> queue_;
  std::uint64_t next_sequence_ = 0;
};

}  // 命名空间 FlightEnvPlatformRuntime
