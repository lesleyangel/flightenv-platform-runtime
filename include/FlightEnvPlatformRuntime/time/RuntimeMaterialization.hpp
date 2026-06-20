#pragma once

/**
 * @file RuntimeMaterialization.hpp
 * @brief 声明公开输出帧的物化策略。
 *
 * 大概：这是把内部事件/计算结果整理成 UI、run package 和 evidence 可见帧的组件。
 * 具体：它决定何时生成 public frame、选择哪些端口输出、如何标记时间和来源。
 * 被谁使用：被 RuntimeTimeScheduler、NativeWorkflowRunner 和 run package 写入路径使用。
 * 使用谁：使用 RuntimeTimeTypes、RuntimeCursor、PortValue 和 evidence/series 契约。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */


#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 根据 JSON 包构建运行时数据面和时间摘要证据。
 *
 * 该辅助工具物化编译后数据面元数据声明的引用和摘要。它不加载张量或制品
 * 内容，也不解释对象特有载荷语义。
 */
class RuntimeMaterialization {
 public:
  /**
   * @brief 为节点输入和输出载荷创建数据面条目。
   * @param data_plane_info 节点的编译后数据面元数据。
   * @param input_payload 供应给适配器的 upstream 输入包。
   * @param output_payload 适配器输出包。
   * @param time_info 本次派发的调度器 time_info。
   * @param iteration_index 公开循环迭代索引。
   * @return 数据面 manifest 条目的 JSON 数组。
   */
  static nlohmann::json makeDataPlaneEntries(
      const nlohmann::json& data_plane_info,
      const nlohmann::json& input_payload,
      const nlohmann::json& output_payload,
      const nlohmann::json& time_info,
      int iteration_index);

  /**
   * @brief 为数据面 point 提取紧凑时间元数据。
   * @param time_info 派发对应的调度器 time_info。
   * @param point 派发输入侧或输出侧的时间点。
   * @return 包含公开输出时间、有效 dt、周期和保持状态的 JSON 对象。
   */
  static nlohmann::json timeSummary(
      const nlohmann::json& time_info,
      const nlohmann::json& point);
};

}  // 命名空间 FlightEnvPlatformRuntime
