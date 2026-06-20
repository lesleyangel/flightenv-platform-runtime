#pragma once

/**
 * @file RuntimeTensorAlignment.hpp
 * @brief 声明大场或张量引用的时间对齐入口。
 *
 * 大概：这是标量对齐之外，为 tensor ref、field ref 留出的扩展接口。
 * 具体：第一阶段可做 nearest/hold，后续可接专用 reducer/interpolator。
 * 被谁使用：被 RuntimeInputAlignment 和未来 tensor 数据面适配器使用。
 * 使用谁：使用 RuntimeTensorInterpolator、DataPlane/PortValue 引用和时间类型。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */


#include "FlightEnvPlatformRuntime/time/RuntimePortSampleBuffer.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 通过引用对齐类张量运行时值的工具。
 *
 * 张量对齐会为下游物化生成惰性操作引用。此头文件边界不加载张量
 * 缓冲区，不检查对象特有字段含义，也不执行数值插值。
 */
class RuntimeTensorAlignment {
 public:
  /**
   * @brief 检测 JSON 值是否携带张量、制品或缓冲区引用。
   * @param value 候选运行时值。
   * @return 当该值可视为引用支撑的类张量数据时为真。
   */
  static bool isTensorLike(const nlohmann::json& value);

  /**
   * @brief 构建惰性的线性插值操作引用。
   * @param before 目标时间或之前的样本。
   * @param after 目标时间或之后的样本。
   * @param target_time_s 目标插值时间，单位为秒。
   * @param channel_id 源通道 id。
   * @return 包含张量 alignment 和 tensor_ref 元数据的 JSON 值。
   */
  static nlohmann::json makeLinearOperationRef(
      const RuntimePortSample& before,
      const RuntimePortSample& after,
      double target_time_s,
      const std::string& channel_id);

  /**
   * @brief 构建惰性的窗口归约操作引用。
   * @param samples 请求窗口内的样本。
   * @param window_start_s 包含端点的窗口起始时间，单位为秒。
   * @param window_end_s 包含端点的窗口结束时间，单位为秒。
   * @param channel_id 源通道 id。
   * @return 包含张量 alignment 和 tensor_ref 元数据的 JSON 值。
   */
  static nlohmann::json makeWindowReductionRef(
      const std::vector<RuntimePortSample>& samples,
      double window_start_s,
      double window_end_s,
      const std::string& channel_id);
};

}  // 命名空间 FlightEnvPlatformRuntime
