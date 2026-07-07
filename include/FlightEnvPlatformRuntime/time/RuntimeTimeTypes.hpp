#pragma once

/**
 * @file RuntimeTimeTypes.hpp
 * @brief 声明 runtime 时间系统共用的时间点、时长、窗口和事件 id 类型。
 *
 * 大概：这是 time 子模块的最底层公共类型文件。
 * 具体：它统一表示 event time、compute time、public time 和时间窗口，避免各组件各自定义时间字段。
 * 被谁使用：被 RuntimeEventQueue、RuntimeNodeClockState、RuntimeInputAlignment、RuntimeMaterialization 使用。
 * 使用谁：只使用标准数值、字符串和比较逻辑，不依赖对象语义。
 * 拆分判断：这是轻量契约头文件，职责清楚；如果以后出现实现逻辑，应移到 runtime 或工具实现文件。
 */


#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

inline constexpr std::int64_t kRuntimeNanosecondsPerSecond = 1000000000LL;

/**
 * @brief runtime 内部使用的稳定时长。
 *
 * 对外 evidence 仍保留秒制 double，内部排序和周期累加使用纳秒整数，避免反复用 double
 * 比较 1/3s、5.5s 这类非整数周期导致边界漂移。
 */
struct RuntimeDuration {
  std::int64_t nanoseconds = 0; ///< 内部统一纳秒时长。

  static RuntimeDuration fromNanoseconds(std::int64_t value) {
    RuntimeDuration duration;
    duration.nanoseconds = value;
    return duration;
  }

  static RuntimeDuration fromSeconds(double seconds) {
    if (!std::isfinite(seconds)) {
      return {};
    }
    return fromNanoseconds(static_cast<std::int64_t>(
        std::llround(seconds * static_cast<double>(kRuntimeNanosecondsPerSecond))));
  }

  double seconds() const {
    return static_cast<double>(nanoseconds) /
           static_cast<double>(kRuntimeNanosecondsPerSecond);
  }

  RuntimeDuration operator*(int multiplier) const {
    return fromNanoseconds(nanoseconds * static_cast<std::int64_t>(multiplier));
  }

  RuntimeDuration operator+(RuntimeDuration rhs) const {
    return fromNanoseconds(nanoseconds + rhs.nanoseconds);
  }

  RuntimeDuration operator-(RuntimeDuration rhs) const {
    return fromNanoseconds(nanoseconds - rhs.nanoseconds);
  }
};

/**
 * @brief runtime 内部使用的稳定时间点。
 */
struct RuntimeTimePoint {
  std::int64_t nanoseconds = 0; ///< 距仿真/运行时钟零点的纳秒数。

  static RuntimeTimePoint fromNanoseconds(std::int64_t value) {
    RuntimeTimePoint point;
    point.nanoseconds = value;
    return point;
  }

  static RuntimeTimePoint fromSeconds(double seconds) {
    return fromNanoseconds(RuntimeDuration::fromSeconds(seconds).nanoseconds);
  }

  double seconds() const {
    return static_cast<double>(nanoseconds) /
           static_cast<double>(kRuntimeNanosecondsPerSecond);
  }

  RuntimeTimePoint operator+(RuntimeDuration duration) const {
    return fromNanoseconds(nanoseconds + duration.nanoseconds);
  }

  RuntimeDuration operator-(RuntimeTimePoint rhs) const {
    return RuntimeDuration::fromNanoseconds(nanoseconds - rhs.nanoseconds);
  }

  bool operator<(RuntimeTimePoint rhs) const { return nanoseconds < rhs.nanoseconds; }
  bool operator==(RuntimeTimePoint rhs) const { return nanoseconds == rhs.nanoseconds; }
};

/**
 * @brief 用于生成运行时 tick 事件的公开循环时间样本。
 */
struct RuntimeLoopTick {
  int iteration_index = 0;              ///< 从零开始的公开循环迭代。
  RuntimeDuration base_dt;              ///< 内部基础步长。
  RuntimeDuration time_offset;          ///< 内部输入侧时间偏移。
  RuntimeTimePoint public_output_time;  ///< 内部公开输出时间点。
  RuntimeDuration output_period;        ///< 内部公开输出周期。
  double base_dt_s = 0.0;               ///< 基础积分步长或公开步长，单位为秒。
  double time_offset_s = 0.0;           ///< tick 开始处的输入侧时间偏移，单位为秒。
  double public_output_time_s = 0.0;    ///< 为此 tick 发出的公开输出时间戳，单位为秒。
  double output_period_s = 0.0;         ///< 有效公开输出周期，单位为秒。

  /**
   * @brief 将 tick 序列化为运行时证据。
   * @return 包含迭代和时间字段的 JSON 对象。
   */
  nlohmann::json toJson() const {
    return {
        {"iteration_index", iteration_index},
        {"base_dt_s", base_dt_s},
        {"base_dt_ns", base_dt.nanoseconds},
        {"time_offset_s", time_offset_s},
        {"time_offset_ns", time_offset.nanoseconds},
        {"public_output_time_s", public_output_time_s},
        {"public_output_time_ns", public_output_time.nanoseconds},
        {"output_period_s", output_period_s},
        {"output_period_ns", output_period.nanoseconds},
    };
  }
};

/**
 * @brief 根据输出周期和基础 dt 输入选择公开周期。
 * @param base_dt_s 基础步长，单位为秒。
 * @param output_period_s 首选公开输出周期，单位为秒。
 * @return 正的输出周期；必要时回退到基础 dt 或一秒。
 */
inline double runtimePublicPeriodS(double base_dt_s, double output_period_s) {
  if (output_period_s > 0.0) {
    return output_period_s;
  }
  return base_dt_s > 0.0 ? base_dt_s : 1.0;
}

/**
 * @brief 将迭代索引转换为输入侧循环偏移。
 * @param iteration_index 从零开始的循环迭代。
 * @param base_dt_s 基础步长，单位为秒。
 * @return 时间偏移，单位为秒。
 */
inline double runtimeLoopTimeOffsetS(int iteration_index, double base_dt_s) {
  return static_cast<double>(iteration_index) * base_dt_s;
}

/**
 * @brief 将迭代索引转换为公开输出时间戳。
 * @param iteration_index 从零开始的循环迭代。
 * @param base_dt_s 基础步长，单位为秒。
 * @return 公开输出时间，单位为秒；dt 非正时回退为 iteration + 1。
 */
inline double runtimeLoopPublicOutputTimeS(int iteration_index, double base_dt_s) {
  if (base_dt_s > 0.0) {
    return runtimeLoopTimeOffsetS(iteration_index, base_dt_s) + base_dt_s;
  }
  return static_cast<double>(iteration_index + 1);
}

/**
 * @brief 计算节点在统一到期栅格上的首个到期时刻（内部纳秒）。
 *
 * 这是 live 事件种子（RuntimeTimeScheduler::seedWorkflowEvents）与审计 trace
 * （RuntimeDueTimeScheduler）共用的唯一首拍规则，避免两处各自实现产生相位分歧：
 * - 输出时间取区间末端：快节点（period <= 公共拍）首拍在一个自身周期处；
 * - 慢节点（period > 公共拍）被拉到第一个公共拍，尽早产出首个样本；
 * - offset 作为整体相位平移叠加在上述基准上。
 *
 * @param period_ns 节点输出周期，纳秒。
 * @param public_period_ns 公共拍周期，纳秒；非正时退化为 period_ns。
 * @param offset_ns 调度表声明的相位偏移，纳秒。
 * @return 首个到期时刻，纳秒。
 */
inline std::int64_t runtimeNodeFirstDueNanoseconds(
    std::int64_t period_ns,
    std::int64_t public_period_ns,
    std::int64_t offset_ns) {
  const std::int64_t base =
      (public_period_ns > 0 && period_ns > public_period_ns) ? public_period_ns : period_ns;
  return offset_ns + base;
}

/**
 * @brief 让节点首拍不早于已知上游首个有效输出。
 *
 * 这仍然是纯平台时间语义：下游节点可以比自己的基础周期晚启动，但不能在直接依赖尚未
 * 产生第一帧之前启动。后续到期仍按节点自身绝对采样栅格推进。
 */
inline std::int64_t runtimeNodeFirstDueAfterDependencyNanoseconds(
    std::int64_t candidate_first_due_ns,
    std::int64_t dependency_first_due_ns) {
  return dependency_first_due_ns > candidate_first_due_ns ? dependency_first_due_ns
                                                          : candidate_first_due_ns;
}

/**
 * @brief 按公开输出周期语义构建循环 tick。
 * @param iteration_index 从零开始的公开循环迭代。
 * @param base_dt_s 基础步长，单位为秒。
 * @param output_period_s 首选公开输出周期，单位为秒。
 * @return 已填充的运行时循环 tick。
 */
inline RuntimeLoopTick makeRuntimeLoopTick(
    int iteration_index,
    double base_dt_s,
    double output_period_s) {
  const double public_period_s = runtimePublicPeriodS(base_dt_s, output_period_s);
  const RuntimeDuration public_period = RuntimeDuration::fromSeconds(public_period_s);
  RuntimeLoopTick tick;
  tick.iteration_index = iteration_index;
  tick.base_dt = RuntimeDuration::fromSeconds(base_dt_s);
  tick.time_offset = public_period * iteration_index;
  tick.public_output_time = RuntimeTimePoint::fromNanoseconds(
      (public_period * (iteration_index + 1)).nanoseconds);
  tick.output_period = public_period;
  tick.base_dt_s = base_dt_s;
  tick.time_offset_s = tick.time_offset.seconds();
  tick.public_output_time_s = tick.public_output_time.seconds();
  tick.output_period_s = public_period_s;
  return tick;
}

/**
 * @brief 单个节点在一次运行时 step 上的派发决策和时间元数据。
 *
 * 该结构由 `RuntimeTimeScheduler` 生成，宿主在决定是否执行节点、保持前一次输出
 * 或附加运行时事件元数据时消费它。
 */
struct RuntimeNodeDispatch {
  bool execute = true;                                      ///< 为真时表示节点应立即派发。
  std::string reason = "every_tick";                        ///< 便于阅读的派发或保持原因。
  RuntimeDuration output_period;                             ///< 内部输出节拍。
  RuntimeTimePoint public_output_time;                       ///< 内部公开输出时间点。
  RuntimeDuration effective_delta_t;                         ///< 内部有效 dt。
  RuntimeTimePoint next_due_time;                            ///< 内部下一次到期时间。
  double output_period_s = 0.0;                              ///< 此节点的有效输出节拍，单位为秒。
  double public_output_time_s = 0.0;                         ///< 已生成或保持输出的公开时间戳。
  double effective_delta_t_s = 0.0;                          ///< 距该节点上一次执行的经过时间。
  double next_due_time_s = std::numeric_limits<double>::quiet_NaN(); ///< 已知时的下一次计划公开时间。
  std::string runtime_event_id;                              ///< 事件驱动时触发本次派发的事件 id。
  std::string runtime_event_kind;                            ///< 事件驱动时触发本次派发的事件类型。
  RuntimeTimePoint runtime_event_time;                       ///< 触发事件内部时间点。
  double runtime_event_time_s = 0.0;                         ///< 触发事件时间，单位为秒。
  int held_from_iteration = -1;                              ///< 被保持输出所属的迭代；无保持时为 -1。
  nlohmann::json time_info = nlohmann::json::object();       ///< 传给适配器和证据的运行时 time_info 包。
  nlohmann::json last_execute_result = nlohmann::json::object(); ///< 为保持输出决策保留的上一次输出。
  nlohmann::json last_time_info = nlohmann::json::object();  ///< 为保持输出证据保留的上一次 time_info。
};

}  // 命名空间 FlightEnvPlatformRuntime
