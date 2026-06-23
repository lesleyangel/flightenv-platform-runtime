#pragma once

/**
 * @file RuntimePortSampleBuffer.hpp
 * @brief 声明端口历史样本缓冲区。
 *
 * 大概：这是输入对齐、最近值查询、线性插值和窗口积分所需的样本窗口。
 * 具体：它按端口保存带时间戳的 PortValue，并提供范围查询、最近样本和清理接口。
 * 被谁使用：被 RuntimeInputAlignment、RuntimeTimeScheduler、NativeWorkflowRunner 和测试使用。
 * 使用谁：使用 RuntimeTimeTypes、PortValue 和标准容器。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */


#include "FlightEnvPlatformRuntime/time/RuntimeTimeTypes.hpp"

#include <nlohmann/json.hpp>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 为输入对齐保留的单个带时间戳节点或端口输出样本。
 */
struct RuntimePortSample {
  std::string channel_id;       ///< 缓冲通道 id，可以是节点 id 或 `node/port`。
  std::string source_node_id;   ///< 产生该样本的节点。
  std::string source_port_id;   ///< 产生该样本的端口；节点级样本为空。
  RuntimeTimePoint time;        ///< 样本内部时间点，运行时比较使用纳秒。
  std::int64_t time_ns = 0;     ///< 样本内部时间戳纳秒，便于 evidence 和审计读取。
  double time_s = 0.0;          ///< 样本的公开输出时间，单位为秒。
  int iteration_index = -1;     ///< 产生该样本的公开迭代。
  nlohmann::json value = nlohmann::json::object();     ///< 输出值或引用包。
  nlohmann::json time_info = nlohmann::json::object(); ///< 与样本关联的调度器 time_info。

  /**
   * @brief 将轻量样本元数据序列化为证据。
   * @return 排除大载荷内容的 JSON 对象。
   */
  nlohmann::json evidence() const;
};

/**
 * @brief 运行时输入对齐使用的内存样本缓冲。
 *
 * 样本在每个通道内按时间排序。该缓冲面向单一运行时循环拥有者；
 * 它不提供内部锁。
 */
class RuntimePortSampleBuffer {
 public:
  /**
   * @brief 返回节点级输出样本的通道 id。
   * @param node_id 源节点标识。
   * @return 等于 `node_id` 的通道 id。
   */
  static std::string nodeChannel(const std::string& node_id);

  /**
   * @brief 返回某个节点输出端口的通道 id。
   * @param node_id 源节点标识。
   * @param port_id 源端口标识；为空时选择节点级通道。
   * @return `node/端口` 形式的通道 id；端口为空时返回节点 id。
   */
  static std::string portChannel(const std::string& node_id, const std::string& port_id);

  /**
   * @brief 记录一个样本，并保持通道按时间排序。
   * @param sample 要移动进缓冲的样本。
   */
  void recordSample(RuntimePortSample sample);

  /**
   * @brief 从适配器结果记录节点级和逐端口样本。
   * @param node_id 源节点标识。
   * @param execute_result 适配器输出包。
   * @param time_info 输出对应的派发 time_info。
   * @param iteration_index 公开迭代索引。
   */
  void recordNodeOutput(
      const std::string& node_id,
      const nlohmann::json& execute_result,
      const nlohmann::json& time_info,
      int iteration_index);

  /**
   * @brief 复制某个通道的全部样本。
   * @param channel_id 要读取的通道。
   * @return 按时间排序的样本向量；通道未知时为空。
   */
  std::vector<RuntimePortSample> samples(const std::string& channel_id) const;

  /**
   * @brief 查找目标时间或之前的最新样本。
   * @param channel_id 要搜索的通道。
   * @param target_time_s 目标时间，单位为秒。
   * @param max_staleness_s 可选最大陈旧时间；负值表示禁用限制。
   * @return 匹配样本；没有样本满足边界时返回 std::nullopt。
   */
  std::optional<RuntimePortSample> latestBeforeOrAt(
      const std::string& channel_id,
      double target_time_s,
      double max_staleness_s = -1.0) const;

  /**
   * @brief 查找最接近目标时间的样本。
   * @param channel_id 要搜索的通道。
   * @param target_time_s 目标时间，单位为秒。
   * @param max_gap_s 可选最大绝对时间间隔；负值表示禁用限制。
   * @return 匹配样本；没有样本满足边界时返回 std::nullopt。
   */
  std::optional<RuntimePortSample> nearest(
      const std::string& channel_id,
      double target_time_s,
      double max_gap_s = -1.0) const;

  /**
   * @brief 查找夹住目标时间的样本。
   * @param channel_id 要搜索的通道。
   * @param target_time_s 目标时间，单位为秒。
   * @return 不晚于目标时间与不早于目标时间的样本对；任一侧都可能为空。
   */
  std::pair<std::optional<RuntimePortSample>, std::optional<RuntimePortSample>> bracketing(
      const std::string& channel_id,
      double target_time_s) const;

  /**
   * @brief 复制闭合时间窗口内的样本。
   * @param channel_id 要搜索的通道。
   * @param start_time_s 包含端点的窗口起始时间，单位为秒。
   * @param end_time_s 包含端点的窗口结束时间，单位为秒。
   * @return 请求窗口内按时间排序的样本。
   */
  std::vector<RuntimePortSample> window(
      const std::string& channel_id,
      double start_time_s,
      double end_time_s) const;

  /**
   * @brief 汇总已缓冲通道和样本数量。
   * @return 排除大样本载荷的 JSON 证据。
   */
  nlohmann::json evidence() const;

 private:
  std::map<std::string, std::vector<RuntimePortSample>> samples_by_channel_;
};

}  // 命名空间 FlightEnvPlatformRuntime
