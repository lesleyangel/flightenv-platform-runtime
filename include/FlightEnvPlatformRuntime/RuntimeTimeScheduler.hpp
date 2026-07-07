#pragma once

/**
 * @file RuntimeTimeScheduler.hpp
 * @brief 声明 runtime 时间调度门面。
 *
 * 大概：这是外部调用者面对的统一时间调度入口，内部细节放到 time 子组件。
 * 具体：它负责把时钟计划、节点节奏、事件队列和输入对齐串起来。
 * 被谁使用：被 NativeWorkflowRunner、PlatformRuntimeHost 和时间调度测试使用。
 * 使用谁：使用 time/RuntimeEventQueue、RuntimeNodeClockState、RuntimeInputAlignment 等组件。
 * 拆分判断：作为 facade 保持合适；后续不要把插值、积分和队列细节继续塞回这里。
 */


#include "FlightEnvPlatformRuntime/time/RuntimeNodeClockState.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeEventQueue.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeTimeTypes.hpp"

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 计算平台级派发节拍和时间证据。
 *
 * 调度器拥有公开 tick timing、节点到期事件、保持输出元数据以及逐节点时钟状态。
 * 它不编码对象语义、适配器行为或 UI 渲染细节。
 *
 * @note 线程安全：实例会在规划和 `markExecuted` 期间修改节点时钟状态；
 * 请为每个运行时循环使用一个调度器，或在外部同步。
 */
class RuntimeTimeScheduler {
 public:
  /**
   * @brief 根据编译后的 time plan 创建调度器。
   * @param time_plan 以 JSON 表示的 time plan，通常是工作流的 `time_plan` 快照。
   */
  explicit RuntimeTimeScheduler(nlohmann::json time_plan = nlohmann::json::object());

  /**
   * @brief 为某个循环偏移上的节点构建基础时间元数据。
   * @param node 编译后的工作流节点元数据。
   * @param iteration_index 公开循环迭代索引。
   * @param time_offset_s 应用于 plan 时间点的时间偏移，单位为秒。
   * @return 节点在派发决策前使用的 JSON time_info 对象。
   */
  nlohmann::json baseTimeInfo(
      const nlohmann::json& node,
      int iteration_index,
      double time_offset_s) const;

  /**
   * @brief 规划节点是否在当前公开 tick 上执行。
   * @param node 编译后的工作流节点元数据。
   * @param iteration_index 公开循环迭代索引。
   * @param base_dt_s 公开基础步长，单位为秒。
   * @param time_offset_s 当前循环时间偏移，单位为秒。
   * @return 派发决策、有效 dt、保持输出元数据和 time_info。
   */
  RuntimeNodeDispatch planDispatch(
      const nlohmann::json& node,
      int iteration_index,
      double base_dt_s,
      double time_offset_s);

  /**
   * @brief 基于显式运行时事件规划节点执行。
   * @param node 编译后的工作流节点元数据。
   * @param event 使节点到期的运行时事件。
   * @param base_dt_s 公开基础步长，单位为秒。
   * @param output_period_s 公开输出周期覆盖值，单位为秒。
   * @return 派发决策和与事件关联的 time_info。
   */
  RuntimeNodeDispatch planEventDispatch(
      const nlohmann::json& node,
      const RuntimeEvent& event,
      double base_dt_s,
      double output_period_s);

  /**
   * @brief 向事件队列写入公开 tick 和节点到期事件。
   * @param events 接收生成事件的队列。
   * @param nodes 按拓扑顺序排列的编译后工作流节点。
   * @param max_iterations 请求的公开迭代范围。
   * @param base_dt_s 公开基础步长，单位为秒。
   * @param output_period_s 公开输出周期覆盖值，单位为秒。
   * @param scheduler_table 可选的编译后 scheduler_table.json；提供时按 dispatch_table
   *        中的 offset_s 对各节点到期栅格做相位平移，并让下游首拍不早于已知
   *        上游首个有效输出。缺省为空对象时仅使用节点自身 depends_on 兼容旧版。
   */
  void seedWorkflowEvents(
      RuntimeEventQueue& events,
      const std::vector<nlohmann::json>& nodes,
      int max_iterations,
      double base_dt_s,
      double output_period_s,
      const nlohmann::json& scheduler_table = nlohmann::json::object()) const;

  /**
   * @brief 将已完成的节点执行记录到逐节点时钟状态。
   * @param node_id 节点标识。
   * @param iteration_index 执行该节点的公开循环迭代。
   * @param dispatch 此调度器返回的派发决策。
   * @param execute_result 为保持输出证据保留的适配器输出包。
   */
  void markExecuted(
      const std::string& node_id,
      int iteration_index,
      const RuntimeNodeDispatch& dispatch,
      const nlohmann::json& execute_result);

  /**
   * @brief 查询为某个节点捕获的调度器状态。
   * @param node_id 节点标识。
   * @return 指向已存状态的指针；节点尚未出现时返回 nullptr。
   */
  const RuntimeNodeClockState* stateFor(const std::string& node_id) const;

  /**
   * @brief 将循环迭代转换为输入侧时间偏移。
   * @param iteration_index 公开循环迭代索引。
   * @param base_dt_s 公开基础步长，单位为秒。
   * @return 时间偏移，单位为秒。
   */
  static double loopTimeOffsetS(int iteration_index, double base_dt_s);

  /**
   * @brief 将循环迭代转换为公开输出时间。
   * @param iteration_index 公开循环迭代索引。
   * @param base_dt_s 公开基础步长，单位为秒。
   * @return 公开输出时间，单位为秒。
   */
  static double loopPublicOutputTimeS(int iteration_index, double base_dt_s);

 private:
  nlohmann::json planNodeInfo(const std::string& node_id) const;
  double nodePeriodS(const nlohmann::json& node, double fallback_period_s) const;
  int publicIterationForTime(double event_time_s, double public_period_s) const;

  nlohmann::json time_plan_ = nlohmann::json::object();
  std::map<std::string, RuntimeNodeClockState> state_by_node_;
};

}  // 命名空间 FlightEnvPlatformRuntime
