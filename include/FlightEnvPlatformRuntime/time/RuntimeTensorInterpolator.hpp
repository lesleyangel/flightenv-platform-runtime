#pragma once

/**
 * @file RuntimeTensorInterpolator.hpp
 * @brief 声明张量或大场数据的插值器接口。
 *
 * 大概：这是后续把 nearest、线性场插值、重采样、窗口 reducer 模块化的扩展点。
 * 具体：当前实现先提供最简单的就近选择，避免把复杂场插值写死在调度器里。
 * 被谁使用：被 RuntimeTensorAlignment 和 tensor 对齐测试使用。
 * 使用谁：使用 RuntimeTimeTypes 和数据面引用，不读取对象专属网格。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */


#include "FlightEnvPlatformRuntime/time/RuntimePortSampleBuffer.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 传给张量插值策略的请求。
 */
struct RuntimeTensorInterpolationRequest {
  std::string operation;                    ///< 请求的操作，例如 linear_interpolate 或 integrate_window。
  std::string channel_id;                   ///< 操作证据中使用的源通道 id。
  double target_time_s = 0.0;               ///< 目标输出时间，单位为秒。
  double window_start_s = 0.0;              ///< 聚合操作的窗口起始时间，单位为秒。
  double window_end_s = 0.0;                ///< 聚合操作的窗口结束时间，单位为秒。
  std::vector<RuntimePortSample> samples;   ///< 候选类 tensor 样本。
};

/**
 * @brief 由张量插值策略返回的结果。
 */
struct RuntimeTensorInterpolationResult {
  bool available = false;                            ///< 为真时，`value` 可应用到已对齐输入。
  std::string status = "not_interpolated";           ///< 机器可读的方法状态。
  std::string method_id;                             ///< 归一化后的插值方法 id。
  double source_time_s = 0.0;                        ///< 选中或合成的源时间，单位为秒。
  int sample_count = 0;                              ///< 已检查的候选样本数量。
  nlohmann::json value = nlohmann::json();           ///< 选中值或惰性 tensor 操作引用。
  nlohmann::json evidence = nlohmann::json::object(); ///< 排除大 tensor 内容的方法证据。
};

/**
 * @brief 用于张量对齐策略的接口。
 *
 * 实现在引用支撑的 JSON 值上工作，并返回选中的引用或惰性操作引用。
 * 除非明确记录，实现在运行时调度路径中不应加载大型张量缓冲区。
 */
class RuntimeTensorInterpolator {
 public:
  /** @brief 释放策略拥有的资源。 */
  virtual ~RuntimeTensorInterpolator() = default;

  /**
   * @brief 返回此策略的稳定方法 id。
   * @return 请求、结果和证据中使用的方法 id。
   */
  virtual std::string methodId() const = 0;

  /**
   * @brief 尝试为一次请求对齐类张量样本。
   * @param request 操作、时间、通道和样本输入。
   * @return 插值结果和证据。
   */
  virtual RuntimeTensorInterpolationResult interpolate(
      const RuntimeTensorInterpolationRequest& request) const = 0;
};

/**
 * @brief 内置张量插值策略的注册表。
 *
 * 注册表是无状态的，只暴露平台支持的方法 id。
 */
class RuntimeTensorInterpolatorRegistry {
 public:
  /**
   * @brief 运行已注册的张量插值策略。
   * @param method_id 请求的方法 id；别名由实现归一化。
   * @param request 用于张量插值的请求。
   * @return 策略结果；没有匹配方法时返回 unsupported 状态。
   */
  static RuntimeTensorInterpolationResult interpolate(
      const std::string& method_id,
      const RuntimeTensorInterpolationRequest& request);

  /**
   * @brief 列出受支持的归一化方法 id。
   * @return 当前运行时构建中可用的方法 id。
   */
  static std::vector<std::string> supportedMethodIds();
};

}  // 命名空间 FlightEnvPlatformRuntime
