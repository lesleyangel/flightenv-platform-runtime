#pragma once

/**
 * @file RuntimeInputAlignment.hpp
 * @brief 声明把端口历史样本对齐到目标时间的策略接口。
 *
 * 大概：这是平台处理 hold_last、nearest、linear、integrate 等输入对齐的公共组件。
 * 具体：它根据目标时间和样本窗口生成 aligned input，不让算子自己猜上一帧或插值。
 * 被谁使用：被 RuntimeTimeScheduler、NativeWorkflowRunner 和端口样本缓冲使用。
 * 使用谁：使用 RuntimePortSampleBuffer、RuntimeTimeTypes、PortValue 和 tensor 对齐接口。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */


#include "FlightEnvPlatformRuntime/time/RuntimePortSampleBuffer.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 将上游样本对齐到节点派发时间所支持的策略。
 */
enum class RuntimeAlignmentStrategy {
  Exact,           ///< 按精确容差语义要求目标时间上存在样本。
  HoldLast,        ///< 在陈旧时间边界内使用目标时间或之前的最新样本。
  Nearest,         ///< 在间隔边界内使用最接近目标时间的样本。
  Linear,          ///< 对标量样本插值，或创建 tensor 插值引用。
  IntegrateWindow, ///< 聚合派发窗口内的样本。
  Independent,     ///< 声明节点输入不依赖上游样本。
  Unsupported,     ///< 解析后的策略不受运行时支持。
};

/**
 * @brief 为一个上游依赖解析出的输入对齐策略。
 */
struct RuntimeInputAlignmentPolicy {
  std::string upstream_node_id; ///< 产生样本的源节点 id。
  std::string source_port_id;   ///< 可选源端口 id；为空时选择节点级通道。
  std::string target_port_id;   ///< 对齐后要填充的可选目标输入端口 id。
  RuntimeAlignmentStrategy strategy = RuntimeAlignmentStrategy::Exact; ///< 归一化后的运行时策略。
  std::string raw_alignment;    ///< 编译后元数据中的原始 `alignment` 值。
  std::string raw_input_resampling; ///< 编译后元数据中的原始 `input_resampling` 值。
  std::string tensor_interpolation = "nearest"; ///< tensor 对齐方法 id，例如 `nearest` 或 `lazy_ref`。
  double max_staleness_s = -1.0; ///< hold-last 策略样本的最大陈旧时间；负值表示禁用限制。
  double max_gap_s = -1.0;       ///< nearest 策略样本的最大距离；负值表示禁用限制。
};

/**
 * @brief 一个输入依赖的对齐结果。
 */
struct RuntimeAlignedInput {
  RuntimeInputAlignmentPolicy policy;                 ///< 用于计算此结果的策略。
  bool available = false;                             ///< 为真时，`value` 可应用到上游包。
  std::string status = "not_aligned";                 ///< 机器可读的对齐状态。
  double target_time_s = 0.0;                         ///< 派发目标时间，单位为秒。
  double source_time_s = 0.0;                         ///< 源样本时间，单位为秒；合成引用可使用目标时间。
  double window_start_s = 0.0;                        ///< 聚合或插值窗口起始。
  double window_end_s = 0.0;                          ///< 聚合或插值窗口结束。
  int sample_count = 0;                               ///< 被考虑的源样本数量。
  nlohmann::json value = nlohmann::json();            ///< 对齐后的 JSON 值或 tensor 引用包。
  nlohmann::json alignment_detail = nlohmann::json::object(); ///< 方法特有证据。

  /**
   * @brief 将对齐结果序列化为运行时证据。
   * @return 包含策略、状态、时间和方法细节的 JSON 对象。
   */
  nlohmann::json evidence() const;
};

/**
 * @brief 节点派发的聚合输入对齐结果。
 */
struct RuntimeInputAlignmentResult {
  std::vector<RuntimeAlignedInput> inputs; ///< 每个解析出的输入对齐策略对应一个结果。

  /**
   * @brief 报告是否应发出任何对齐证据。
   * @return 至少评估过一个输入策略时为真。
   */
  bool hasEvidence() const;

  /**
   * @brief 序列化所有已对齐输入的证据。
   * @return 由 `RuntimeAlignedInput::evidence()` 对象组成的 JSON 数组。
   */
  nlohmann::json evidence() const;
};

/**
 * @brief 从运行时样本缓冲对齐节点输入的无状态辅助工具。
 *
 * 这些辅助工具不执行适配器，也不检查对象特有语义；它们只把声明的输入对齐策略
 * 映射为样本缓冲查询和证据更新。
 */
class RuntimeInputAlignment {
 public:
  /**
   * @brief 将节点声明的输入对齐到当前 time_info。
   * @param node 编译后的工作流节点元数据。
   * @param time_info 调度器生成的派发时间信息。
   * @param sample_buffer 包含既往节点或端口输出的运行时样本缓冲。
   * @return `time_info` 或 `node` 中每个解析策略对应的对齐结果。
   */
  static RuntimeInputAlignmentResult alignNodeInputs(
      const nlohmann::json& node,
      const nlohmann::json& time_info,
      const RuntimePortSampleBuffer& sample_buffer);

  /**
   * @brief 将可用的已对齐输入应用到上游 JSON 包。
   * @param result 要追加并应用的对齐结果。
   * @param upstream 要修改的上游包；会写入 `runtime_input_alignment` 证据和
   * 可选目标输入端口值。
   */
  static void applyAlignedInputs(
      const RuntimeInputAlignmentResult& result,
      nlohmann::json& upstream);
};

}  // 命名空间 FlightEnvPlatformRuntime
